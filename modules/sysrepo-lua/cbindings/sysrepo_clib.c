/*  Copyright (C) 2020 CZ.NIC, z.s.p.o. <knot-resolver@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sysrepo_clib.h"

#include <stdio.h>
#include <stdlib.h>
#include <sysrepo.h>
#include <uv.h>
#include <libyang/libyang.h>

#include "lib/module.h"
#include "common/sysrepo_conf.h"

EXPORT_STRDEF_TO_LUA_IMPL(YM_COMMON)
EXPORT_STRDEF_TO_LUA_IMPL(XPATH_BASE)

struct el_subscription_ctx {
	sr_conn_ctx_t *connection;
	sr_session_ctx_t *session;
	sr_subscription_ctx_t *subscription;
	el_subsription_cb callback;
	uv_poll_t uv_handle;
};

/** Callback to Lua used for applying configuration  */
static apply_conf_f apply_conf = NULL;
/** Callback to Lua used for reading configuration */
static read_conf_f read_conf = NULL;
static el_subscription_ctx_t *el_subscription_ctx = NULL;
static const struct lys_node* lys_root = NULL;
static const struct ly_ctx* ly_context = NULL;

/**
 * Change callback getting called by sysrepo. Iterates over changed options and passes
 * them over to Lua.
 */
static int sysrepo_conf_change_callback(sr_session_ctx_t *session,
					const char *module_name,
					const char *xpath, sr_event_t event,
					uint32_t request_id, void *private_data)
{
	if (event == SR_EV_CHANGE) {
		// gets called before the actual change of configuration is commited. If we
		// return an error, the change is aborted can be used for configuration
		// verification. Must have no sideeffects.

		// TODO
	} else if (event == SR_EV_ABORT) {
		// Gets called when the transaction gets aborted. Because we have no
		// sideeffects during verification, we don't care and and there is nothing
		// to do
	} else if (event == SR_EV_DONE) {
		// after configuration change commit. We should apply the configuration.
		// Will not hurt if we verify the changes again, but we have no way of
		// declining the change now

		int sr_err = SR_ERR_OK;
		struct lyd_node* tree;

		// get the whole config tree
		sr_err = sr_get_data(session, XPATH_BASE, 0, 0, 0, &tree);
		if (sr_err != SR_ERR_OK)
			goto cleanup;

		// apply the configuration
		apply_conf(tree);
	cleanup:
		if (sr_err != SR_ERR_OK && sr_err != SR_ERR_NOT_FOUND)
			kr_log_error("Sysrepo module error: %s\n",
				     sr_strerror(sr_err));
		lyd_free_withsiblings(tree); // FIXME sure about this?
	}
	return SR_ERR_OK;
}

static int sysrepo_conf_read_callback(sr_session_ctx_t *session, const char *module_name, const char *path, const char *request_xpath, uint32_t request_id, struct lyd_node **parent, void *private_data) {
	// *parent is expected to be NULL, because we are subscribed only on the top-level node
	assert(*parent == NULL);
	// so we can safely ignore it and overwrite it with our new tree
	struct lyd_node *res = read_conf();
	if (res == NULL)
		return SR_ERR_CALLBACK_FAILED;
	
	*parent = res;
	return SR_ERR_OK;
}

void el_subscr_finish_closing(uv_handle_t *handle)
{
	el_subscription_ctx_t *el_subscr = handle->data;
	assert(el_subscr != NULL);
	free(el_subscr);
}

/** Free a event loop subscription. */
void el_subscription_free(el_subscription_ctx_t *el_subscr)
{
	sr_disconnect(el_subscr->connection);
	uv_close((uv_handle_t *)&el_subscr->uv_handle,
		 el_subscr_finish_closing);
}

static void el_subscr_cb_tramp(uv_poll_t *handle, int status, int events)
{
	el_subscription_ctx_t *el_subscr = handle->data;
	el_subscr->callback(el_subscr, status);
}

/** Start a new event loop subscription.  */
static el_subscription_ctx_t *
el_subscription_new(sr_subscription_ctx_t *sr_subscr,
		    el_subsription_cb el_callback)
{
	int fd;
	int err = sr_get_event_pipe(sr_subscr, &fd);
	if (err != SR_ERR_OK)
		return NULL;
	el_subscription_ctx_t *el_subscr = malloc(sizeof(*el_subscr));
	if (el_subscr == NULL)
		return NULL;
	err = uv_poll_init(uv_default_loop(), &el_subscr->uv_handle, fd);
	if (err != 0) {
		free(el_subscr);
		return NULL;
	}
	el_subscr->subscription = sr_subscr;
	el_subscr->callback = el_callback;
	el_subscr->uv_handle.data = el_subscr;
	err = uv_poll_start(&el_subscr->uv_handle, UV_READABLE,
			    el_subscr_cb_tramp);
	if (err != 0) {
		el_subscription_free(el_subscr);
		return NULL;
	}
	return el_subscr;
}

static void el_subscr_cb(el_subscription_ctx_t *el_subscr, int status)
{
	if (status) {
		/* some error */
		return;
	}
	/* normal state */
	sr_process_events(el_subscr->subscription, el_subscr->session, NULL);
}

int sysrepo_init(apply_conf_f apply_conf_callback, read_conf_f read_conf_callback)
{
	// store callback to Lua
	apply_conf = apply_conf_callback;
	read_conf = read_conf_callback;

	int sr_err = SR_ERR_OK;
	sr_conn_ctx_t *sr_connection = NULL;
	sr_session_ctx_t *sr_session = NULL;
	sr_subscription_ctx_t *sr_subscription = NULL;

	sr_err = sr_connect(0, &sr_connection);
	if (sr_err != SR_ERR_OK)
		goto cleanup;

	sr_err = sr_session_start(sr_connection, SR_DS_RUNNING, &sr_session);
	if (sr_err != SR_ERR_OK)
		goto cleanup;

	/* obtain schema root node, will be used from within Lua */
	ly_context = sr_get_context(sr_connection);
	assert(ly_context != NULL);
	lys_root = ly_ctx_get_node(ly_context, NULL, XPATH_BASE, 0);
	assert(lys_root != NULL);

	/* register sysrepo subscription and callback
		SR_SUBSCR_NO_THREAD - don't create a thread for handling them
		SR_SUBSCR_ENABLED - send us current configuration in a callback just after subscribing
	 */
	sr_err = sr_module_change_subscribe(
		sr_session, YM_COMMON, XPATH_BASE, sysrepo_conf_change_callback,
		NULL, 0, SR_SUBSCR_NO_THREAD | SR_SUBSCR_ENABLED,
		&sr_subscription);
	if (sr_err != SR_ERR_OK)
		goto cleanup;

	sr_err = sr_oper_get_items_subscribe(
		sr_session, YM_COMMON, XPATH_BASE, sysrepo_conf_read_callback,
		NULL, SR_SUBSCR_CTX_REUSE | SR_SUBSCR_NO_THREAD,
		&sr_subscription
	);
	if (sr_err != SR_ERR_OK)
		goto cleanup;

	/* add subscriptions to kres event loop */
	el_subscription_ctx =
		el_subscription_new(sr_subscription, el_subscr_cb);
	el_subscription_ctx->connection = sr_connection;
	el_subscription_ctx->session = sr_session;

	return kr_ok();

cleanup:
	sr_disconnect(sr_connection);
	kr_log_error("Error (%s)\n", sr_strerror(sr_err));
	return kr_error(sr_err);
}

int sysrepo_deinit()
{
	el_subscription_free(el_subscription_ctx);
	
	// remove reference to Lua callback so that it can be free'd safely
	apply_conf = NULL;
	read_conf = NULL;

	return kr_ok();
}

KR_EXPORT struct lyd_node* node_child_first(struct lyd_node* parent) {
	assert(parent != NULL);

	/* return null when the node does not contain children */
	if(
		parent->schema->nodetype != LYS_CONTAINER &&
		parent->schema->nodetype != LYS_LIST &&
		parent->schema->nodetype != LYS_CHOICE
	) {
		return NULL;
	}

	return parent->child;
}

KR_EXPORT struct lyd_node* node_child_next(struct lyd_node* prev_child) {
	assert(prev_child != NULL);
	return prev_child->next;
}

KR_EXPORT const char* node_get_name(struct lyd_node* node) {
	assert(node != NULL);
	return node->schema->name;
}

KR_EXPORT const char* node_get_value_str(struct lyd_node* node) {
	assert(node != NULL);

	assert(
		node->schema->nodetype ==  LYS_LEAF ||
		node->schema->nodetype == LYS_LEAFLIST
	);

	return ((struct lyd_node_leaf_list*) node)->value_str;
}

KR_EXPORT struct lyd_node* node_new_leaf(struct lyd_node* parent, const struct lys_module* module, const char* name, const char* value) {
	return lyd_new_leaf(parent, module, name, value);
}
KR_EXPORT struct lyd_node* node_new_container(struct lyd_node* parent, const struct lys_module* module, const char* name) {
	return lyd_new(parent, module, name);
}
KR_EXPORT const struct lys_module* schema_get_module(const struct lys_node* schema) {
	assert(schema != NULL);
	return schema->module;
}

KR_EXPORT const struct lys_node* schema_child_first(const struct lys_node* parent) {
	assert(parent != NULL);

	/* if the node does not contain children, return NULL */
	if (
		parent->nodetype != LYS_CONTAINER &&
		parent->nodetype != LYS_LIST &&
		parent->nodetype != LYS_CHOICE
	) {
		return NULL;
	}

	return lys_getnext(NULL, parent, NULL, 0);
}

KR_EXPORT const struct lys_node* schema_child_next(const struct lys_node* parent, const struct lys_node* prev_child) {
	assert(prev_child != NULL);
	assert(parent != NULL);

	return lys_getnext(prev_child, parent, NULL, 0);
}

KR_EXPORT const char* schema_get_name(const struct lys_node* node) {
	assert(node != NULL);
	return node->name;
}

KR_EXPORT const struct lys_node* schema_root() {
	assert(lys_root != NULL);
	return lys_root;
}

KR_EXPORT int node_validate(struct lyd_node* node) {
	assert(node != NULL);
	assert(ly_context != NULL);
	return lyd_validate(&node, LYD_OPT_DATA | LYD_OPT_STRICT | LYD_OPT_DATA_NO_YANGLIB, (void*) ly_context);
}

KR_EXPORT void node_free(struct lyd_node* node) {
	lyd_free_withsiblings(node);
}

KR_EXPORT bool node_is_list_item(struct lyd_node* node) {
	assert(node != NULL);

	return node->schema->nodetype == LYS_LIST || node->schema->nodetype == LYS_LEAFLIST;
}

KR_EXPORT bool node_is_container(struct lyd_node* node) {
	assert(node != NULL);

	return node->schema->nodetype == LYS_CONTAINER;
}

KR_EXPORT bool node_is_leaf(struct lyd_node* node) {
	assert(node != NULL);

	return node->schema->nodetype == LYS_LEAF || node->schema->nodetype == LYS_LEAFLIST;
}

KR_EXPORT bool node_is_number_type(struct lyd_node* node) {
	assert(node != NULL);
	assert(node->schema->nodetype == LYS_LEAF || node->schema->nodetype == LYS_LEAFLIST);

	LY_DATA_TYPE type = ((struct lyd_node_leaf_list*) node)->value_type;
	switch (type) {
		case LY_TYPE_INT8:
		case LY_TYPE_UINT8:
		case LY_TYPE_INT16:
		case LY_TYPE_UINT16:
		case LY_TYPE_INT32:
		case LY_TYPE_UINT32:
		case LY_TYPE_INT64:
		case LY_TYPE_UINT64:
			return true;
		default:
			return false;
	}
}
