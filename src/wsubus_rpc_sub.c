/*
 * Copyright (C) 2016 Inteno Broadband Technology AB
 *
 * This software is the confidential and proprietary information of the
 * Inteno Broadband Technology AB. You shall not disclose such Confidential
 * Information and shall use it only in accordance with the terms of the
 * license agreement you entered into with the Inteno Broadband Technology AB
 *
 * All rights reserved.
 *
 * Author: Denis Osvald <denis.osvald@sartura.hr>
 *
 */
/*
 * ubus over websocket - ubus event subscription
 */
#include "wsubus_rpc_sub.h"

#include "common.h"
#include "wsubus.impl.h"
#include "wsubus_rpc.h"
#include "wsubus_access_check.h"

#include <libubox/blobmsg_json.h>
#include <libubox/blobmsg.h>
#include <libubus.h>

#include <libwebsockets.h>

#include <assert.h>

struct wsubus_sub_info {
	uint32_t sub_id;

	struct list_head list;

	struct blob_attr *src_blob;
	const char *pattern;

	struct ubus_event_handler ubus_handler;

	struct lws *wsi;
};

static struct wsubus_sub_info list_of_subscriptions = { .list = LIST_HEAD_INIT(list_of_subscriptions.list) };

static void wsubus_sub_cb(struct ubus_context *ctx, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg);

static void wsubus_unsub_elem(struct wsubus_sub_info *elem)
{
	struct prog_context *prog = lws_context_user(lws_get_context(elem->wsi));
	ubus_unregister_event_handler(prog->ubus_ctx, &elem->ubus_handler);
	free(elem->src_blob);
	list_del(&elem->list);
	free(elem);
}

void wsubus_clean_all_subscriptions(void)
{
	int count = 0;
	struct wsubus_sub_info *elem, *tmp;

	list_for_each_entry_safe(elem, tmp, &list_of_subscriptions.list, list) {
		free(elem->src_blob);
		list_del(&elem->list);
		free(elem);
		++count;
	}
	if (count)
		lwsl_warn("%d subscriptions cleaned at exit\n", count);
}

int wsubus_unsubscribe_by_wsi_and_id(struct lws *wsi, uint32_t id)
{
	struct wsubus_sub_info *elem, *tmp;
	int ret = 1;

	list_for_each_entry_safe(elem, tmp, &list_of_subscriptions.list, list) {
		// check id
		if (elem->wsi == wsi && elem->sub_id == id) {
			wsubus_unsub_elem(elem);
			ret = 0;
		}
	}
	return ret;
}

int wsubus_unsubscribe_by_wsi_and_pattern(struct lws *wsi, const char *pattern)
{
	struct wsubus_sub_info *elem, *tmp;
	int ret = 1;

	list_for_each_entry_safe(elem, tmp, &list_of_subscriptions.list, list) {
		// check sid, pattern
		if (elem->wsi == wsi && !strcmp(pattern, elem->pattern)) {
			wsubus_unsub_elem(elem);
			elem = NULL;
			ret = 0;
		}
	}
	return ret;
}

int wsubus_unsubscribe_all_by_wsi(struct lws *wsi)
{
	struct wsubus_sub_info *elem, *tmp;
	int ret = 1;

	list_for_each_entry_safe(elem, tmp, &list_of_subscriptions.list, list) {
		// check sid
		if (elem->wsi == wsi) {
			wsubus_unsub_elem(elem);
			elem = NULL;
			ret = 0;
		}
	}
	return ret;
}

int ubusrpc_blob_sub_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob)
{
	static const struct blobmsg_policy rpc_ubus_param_policy[] = {
		[0] = { .type = BLOBMSG_TYPE_STRING }, // ubus-session id
		[1] = { .type = BLOBMSG_TYPE_STRING }, // ubus-object
	};
	enum { __RPC_U_MAX = (sizeof rpc_ubus_param_policy / sizeof rpc_ubus_param_policy[0]) };
	struct blob_attr *tb[__RPC_U_MAX];

	struct blob_attr *dup_blob = blob_memdup(blob);
	if (!dup_blob) {
		return -100;
	}

	// TODO<blob> blob_(data|len) vs blobmsg_xxx usage, what is the difference
	// and which is right here? (uhttpd ubus uses blobmsg_data for blob which
	// comes from another blob's table... here and so do we)
	blobmsg_parse_array(rpc_ubus_param_policy, __RPC_U_MAX, tb, blobmsg_data(dup_blob), (unsigned)blobmsg_len(dup_blob));

	if (!tb[0])
		return -1;
	if (!tb[1])
		return -2;

	ubusrpc->sub.src_blob = dup_blob;
	ubusrpc->sub.sid = tb[0] ? blobmsg_get_string(tb[0]) : UBUS_DEFAULT_SID;
	ubusrpc->sub.pattern = blobmsg_get_string(tb[1]);

	return 0;
}

int ubusrpc_blob_sub_list_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob)
{
	static const struct blobmsg_policy rpc_ubus_param_policy[] = {
		[0] = { .type = BLOBMSG_TYPE_STRING }, // ubus-session id
	};
	enum { __RPC_U_MAX = (sizeof rpc_ubus_param_policy / sizeof rpc_ubus_param_policy[0]) };
	struct blob_attr *tb[__RPC_U_MAX];

	// TODO<blob> blob_(data|len) vs blobmsg_xxx usage, what is the difference
	// and which is right here? (uhttpd ubus uses blobmsg_data for blob which
	// comes from another blob's table... here and so do we)
	blobmsg_parse_array(rpc_ubus_param_policy, __RPC_U_MAX, tb, blobmsg_data(blob), (unsigned)blobmsg_len(blob));

	if (!tb[0])
		return 2;

	ubusrpc->sub.src_blob = NULL;
	ubusrpc->sub.sid = tb[0] ? blobmsg_get_string(tb[0]) : UBUS_DEFAULT_SID;

	return 0;
}

int ubusrpc_blob_unsub_by_id_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob)
{
	static const struct blobmsg_policy rpc_ubus_param_policy[] = {
		[0] = { .type = BLOBMSG_TYPE_STRING }, // ubus-session id
		[1] = { .type = BLOBMSG_TYPE_INT32 }, // subscribe id
	};
	enum { __RPC_U_MAX = (sizeof rpc_ubus_param_policy / sizeof rpc_ubus_param_policy[0]) };
	struct blob_attr *tb[__RPC_U_MAX];

	// TODO<blob> blob_(data|len) vs blobmsg_xxx usage, what is the difference
	// and which is right here? (uhttpd ubus uses blobmsg_data for blob which
	// comes from another blob's table... here and so do we)
	blobmsg_parse_array(rpc_ubus_param_policy, __RPC_U_MAX, tb, blobmsg_data(blob), (unsigned)blobmsg_len(blob));

	if (!tb[0])
		return 2;
	if (!tb[1])
		return 2;

	ubusrpc->unsub_by_id.src_blob = NULL;
	ubusrpc->unsub_by_id.sid = tb[0] ? blobmsg_get_string(tb[0]) : UBUS_DEFAULT_SID;
	ubusrpc->unsub_by_id.id = blobmsg_get_u32(tb[1]);

	return 0;
}


int ubusrpc_handle_sub(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id)
{
	int ret;

	struct wsubus_sub_info *subinfo = malloc(sizeof *subinfo);
	if (!subinfo) {
		lwsl_err("alloc subinfo error\n");
		ret = UBUS_STATUS_UNKNOWN_ERROR;
		goto out;
	}

	struct wsubus_client_session *client = lws_wsi_user(wsi);
	// TODO check_and_update_sid should go in one common place
	if (wsubus_check_and_update_sid(client, ubusrpc->sub.sid) != 0) {
		lwsl_warn("curr sid %s != prev sid %s\n", ubusrpc->sub.sid, client->last_known_sid);
		ret = UBUS_STATUS_NOT_SUPPORTED;
		free(subinfo);
		goto out;
	}

	subinfo->ubus_handler = (struct ubus_event_handler){};

	lwsl_notice("client %u wants to do ubus listen %s\n", client->id, ubusrpc->sub.pattern);

	struct prog_context *prog = lws_context_user(lws_get_context(wsi));
	ret = ubus_register_event_handler(prog->ubus_ctx, &subinfo->ubus_handler, ubusrpc->sub.pattern);

	if (ret) {
		lwsl_err("ubus reg evh error %s\n", ubus_strerror(ret));
		free(subinfo);
		goto out;
	}

	static uint32_t subscribe_id = 1;
	subinfo->ubus_handler.cb = wsubus_sub_cb;

	subinfo->sub_id = subscribe_id++;
	subinfo->src_blob = ubusrpc->sub.src_blob;
	subinfo->pattern = ubusrpc->sub.pattern;
	// subinfo->ubus_handler inited above in ubus_register_...
	subinfo->wsi = wsi;

	ubusrpc->src_blob = NULL;

	list_add_tail(&subinfo->list, &list_of_subscriptions.list);

out:
	if (ret) {
		free(ubusrpc->sub.src_blob);
		ubusrpc->src_blob = NULL;
	}
	char *response = jsonrpc_response_from_blob(id, ret, NULL);
	wsubus_write_response_str(wsi, response);
	free(response);
	free(ubusrpc);

	return 0;
}

static void blobmsg_add_sub_info(struct blob_buf *buf, const char *name, const struct wsubus_sub_info *sub)
{
	void *tkt = blobmsg_open_table(buf, name);

	blobmsg_add_string(buf, "pattern", sub->pattern);
	blobmsg_add_u32(buf, "id", sub->sub_id);

	blobmsg_close_table(buf, tkt);
}

int ubusrpc_handle_sub_list(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id)
{
	char *response_str;
	int ret = 0;

	struct wsubus_client_session *client = lws_wsi_user(wsi);
	// TODO check_and_update_sid should go in one common place
	if (wsubus_check_and_update_sid(client, ubusrpc->sub.sid) != 0) {
		ret = UBUS_STATUS_NOT_SUPPORTED;
		goto out;
	}

	lwsl_notice("client %u wants to list listens\n", client->id);

	struct blob_buf sub_list_blob = {};
	blob_buf_init(&sub_list_blob, 0);

	void* array_ticket = blobmsg_open_array(&sub_list_blob, "");
	struct wsubus_sub_info *elem, *tmp;
	list_for_each_entry_safe(elem, tmp, &list_of_subscriptions.list, list) {
		if (elem->wsi == wsi)
			blobmsg_add_sub_info(&sub_list_blob, "", elem);
	}
	blobmsg_close_array(&sub_list_blob, array_ticket);

out:
	if (ret) {
		response_str = jsonrpc_response_from_blob(id, ret, NULL);
	} else {
		// using blobmsg_data here to pass only array part of blobmsg
		response_str = jsonrpc_response_from_blob(id, 0, blobmsg_data(sub_list_blob.head));
		blob_buf_free(&sub_list_blob);
	}

	wsubus_write_response_str(wsi, response_str);

	// free memory
	free(response_str);
	free(ubusrpc->sub.src_blob);
	free(ubusrpc);
	return 0;
}

int ubusrpc_handle_unsub_by_id(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id)
{
	char *response;
	int ret = 0;

	struct wsubus_client_session *client = lws_wsi_user(wsi);
	// TODO check_and_update_sid should go in one common place
	if (wsubus_check_and_update_sid(client, ubusrpc->unsub_by_id.sid) != 0) {
		ret = UBUS_STATUS_NOT_SUPPORTED;
		goto out;
	}

	lwsl_notice("client %u wants to unlisten id %u\n", client->id, ubusrpc->unsub_by_id.id);
	ret = wsubus_unsubscribe_by_wsi_and_id(wsi, ubusrpc->unsub_by_id.id);

	if (ret != 0)
		ret = UBUS_STATUS_NOT_FOUND;

out:
	response = jsonrpc_response_from_blob(id, ret, NULL);
	wsubus_write_response_str(wsi, response);
	free(response);
	free(ubusrpc);

	return 0;
}

int ubusrpc_handle_unsub(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id)
{
	char *response;
	int ret = 0;

	struct wsubus_client_session *client = lws_wsi_user(wsi);
	// TODO check_and_update_sid should go in one common place
	if (wsubus_check_and_update_sid(client, ubusrpc->sub.sid) != 0) {
		ret = UBUS_STATUS_NOT_SUPPORTED;
		goto out;
	}

	lwsl_notice("client %u wants to unlisten %s\n", client->id, ubusrpc->sub.pattern);
	ret = wsubus_unsubscribe_by_wsi_and_pattern(wsi, ubusrpc->sub.pattern);

	if (ret != 0)
		ret = UBUS_STATUS_NOT_FOUND;

out:
	response = jsonrpc_response_from_blob(id, ret, NULL);
	wsubus_write_response_str(wsi, response);
	free(response);
	free(ubusrpc->sub.src_blob);
	free(ubusrpc);

	return 0;
}

struct wsubus_ev_notif {
	char *type;
	struct blob_attr *msg;
	struct wsubus_sub_info *sub;
	struct wsubus_client_access_check_ctx cr;
};

static void wsubus_ev_destroy_ctx(struct wsubus_ev_notif *t)
{
	free(t->type);
	free(t->msg);
	free(t);
}

static void wsubus_ev_check__destroy(struct wsubus_client_access_check_ctx *cr)
{
	wsubus_ev_destroy_ctx(container_of(cr, struct wsubus_ev_notif, cr));
};

static void wsubus_ev_check_cb(struct wsubus_access_check_req *req, void *ctx, bool access)
{
	struct wsubus_ev_notif *t = ctx;

	assert(req == t->cr.req);

	if (!access) {
		lwsl_notice("access check for event %s denied\n", t->type);
		goto out;
	}

	// manually construct jsonrpc since we're only one sending not resp but req
	struct blob_buf resp_buf = {};
	blob_buf_init(&resp_buf, 0);
	blobmsg_add_string(&resp_buf, "jsonrpc", "2.0");
	blobmsg_add_string(&resp_buf, "method", "event");

	void *tkt = blobmsg_open_table(&resp_buf, "params");
	blobmsg_add_string(&resp_buf, "type", t->type);
	blobmsg_add_field(&resp_buf, BLOBMSG_TYPE_TABLE, "data", blobmsg_data(t->msg), blobmsg_len(t->msg));
	blobmsg_add_sub_info(&resp_buf, "subscription", t->sub);
	blobmsg_close_table(&resp_buf, tkt);

	char *response = blobmsg_format_json(resp_buf.head, true);
	blob_buf_free(&resp_buf);

	wsubus_write_response_str(t->sub->wsi, response);
	free(response);

out:
	list_del(&t->cr.acq);
	wsubus_ev_destroy_ctx(t);
}

static void wsubus_sub_cb(struct ubus_context *ctx, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg)
{
	__attribute__((unused)) int mtype = blobmsg_type(msg);
	lwsl_debug("sub cb called, ev type %s, blob of len %lu thpe %s\n", type, blobmsg_len(msg),
			mtype == BLOBMSG_TYPE_STRING ? "\"\"" :
			mtype == BLOBMSG_TYPE_TABLE ? "{}" :
			mtype == BLOBMSG_TYPE_ARRAY ? "[]" : "<>");

	struct wsubus_sub_info *sub = container_of(ev, struct wsubus_sub_info, ubus_handler);
	struct wsubus_client_session *client = lws_wsi_user(sub->wsi);

	struct wsubus_ev_notif *t = malloc(sizeof *t);
	t->type = strdup(type);
	t->msg = blob_memdup(msg);
	t->sub = sub;
	t->cr.req = wsubus_access_check__event(ctx, type, client->last_known_sid, t, wsubus_ev_check_cb);

	if (!t->cr.req) {
		wsubus_ev_destroy_ctx(t);
		return;
	}

	t->cr.destructor = wsubus_ev_check__destroy;
	list_add_tail(&t->cr.acq, &client->access_check_q);
}
