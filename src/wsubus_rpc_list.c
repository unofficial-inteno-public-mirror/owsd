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
 * ubus over websocket - ubus list
 */
#include "wsubus_rpc_list.h"

#include "common.h"
#include "wsubus.impl.h"
#include "wsubus_rpc.h"

#include <libubox/blobmsg_json.h>
#include <libubox/blobmsg.h>
#include <libubus.h>

#include <libwebsockets.h>

#include <assert.h>

int ubusrpc_blob_list_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob)
{
	if (blob_id(blob) != BLOBMSG_TYPE_ARRAY) {
		ubusrpc->list.src_blob = NULL;
		ubusrpc->list.pattern = NULL;
		return 0;
	}

	static const struct blobmsg_policy rpc_ubus_param_policy[] = {
		[0] = { .type = BLOBMSG_TYPE_UNSPEC }, // session ID, IGNORED to keep compat
		[1] = { .type = BLOBMSG_TYPE_STRING }, // ubus-object pattern
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

	if (!tb[1])
		return -2;

	ubusrpc->list.src_blob = dup_blob;
	ubusrpc->list.pattern = blobmsg_get_string(tb[1]);

	return 0;
}

struct list_cb_data {
	int error;
	struct blob_buf buf;
};

static void ubus_lookup_cb(struct ubus_context *ctx, struct ubus_object_data *obj, void *user)
{
	static const char *const blobmsg_type_to_str[] = {
		[BLOBMSG_TYPE_ARRAY] = "array",
		[BLOBMSG_TYPE_TABLE] = "object",
		[BLOBMSG_TYPE_STRING] = "string",
		[BLOBMSG_TYPE_INT32] = "number",
		[BLOBMSG_TYPE_INT16] = "number",
		[BLOBMSG_TYPE_BOOL] = "boolean",
	};

	(void)ctx;

	lwsl_info("looked up %s\n", obj->path);
	struct list_cb_data *data = user;

	void *objs_tkt = blobmsg_open_table(&data->buf, obj->path);

	if (!obj->signature) {
		goto out;
	}

	unsigned int r_methods;
	struct blob_attr *cur_method;

	blob_for_each_attr(cur_method, obj->signature, r_methods) {
		void *methods_tkt = blobmsg_open_table(&data->buf, blobmsg_name(cur_method));

		struct blob_attr *cur_arg;
		unsigned r_args = (unsigned)blobmsg_len(cur_method);
		__blob_for_each_attr(cur_arg, blobmsg_data(cur_method), r_args) {
			if (blobmsg_type(cur_arg) != BLOBMSG_TYPE_INT32)
				continue;
			const char *typestr = blobmsg_type_to_str[blobmsg_get_u32(cur_arg)];
			typestr = typestr ? typestr : "unknown";
			blobmsg_add_string(&data->buf, blobmsg_name(cur_arg), typestr);
		}

		blobmsg_close_table(&data->buf, methods_tkt);
	}
out:
	blobmsg_close_table(&data->buf, objs_tkt);
}

int ubusrpc_handle_list(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id)
{
	char *response_str;
	int ret = 0;

	struct list_cb_data list_data = {1, {}};
	blob_buf_init(&list_data.buf, 0);

	struct prog_context *prog = lws_context_user(lws_get_context(wsi));
	struct wsubus_client_session *client = lws_wsi_user(wsi);

	void *results_ticket = blobmsg_open_table(&list_data.buf, "");
	lwsl_notice("client %u wants to do ubus list %s\n", client->id, ubusrpc->list.pattern);
	ret = ubus_lookup(prog->ubus_ctx, ubusrpc->list.pattern, ubus_lookup_cb, &list_data);
	if (list_data.error || ret)
		lwsl_warn("after loookup rc %d, error %d\n", ret, list_data.error);
	blobmsg_close_table(&list_data.buf, results_ticket);

	if (ret) {
		response_str = jsonrpc_response_from_blob(id, ret ? ret : -1, NULL);
	} else {
		// using blobmsg_data here to pass only array part of blobmsg
		response_str = jsonrpc_response_from_blob(id, 0, blobmsg_data(list_data.buf.head));
	}

	blob_buf_free(&list_data.buf);

	wsubus_write_response_str(wsi, response_str);

	// free memory
	free(response_str);
	free(ubusrpc->list.src_blob);
	free(ubusrpc);
	return 0;
}

