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
 * ubus over websocket - client session and message handling
 */
#include "common.h"
#include "wifiimport.h"
#include "wsubus.h"

#include <libubox/uloop.h>
#include <libubox/blobmsg_json.h>
#include <libubox/avl-cmp.h>
#include <json-c/json.h>

#include <libwebsockets.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

#define WSUBUS_PROTO_NAME "ubus-json"

#define MAX_INFLIGHT_CALLS 20

#define find_first_zero(x) ffs(~(x))

static lws_callback_function wsubus_cb;

struct remote_ubus {
	int call_id;
	char sid[64];

	struct {
		unsigned int login  : 1;
		unsigned int listen : 1;
		unsigned int list   : 1;
		unsigned int call   : MAX_INFLIGHT_CALLS;
	} waiting_for;

	struct proxied_call {
		int jsonrpc_id;
		struct ubus_request_data ureq;
	} calls[MAX_INFLIGHT_CALLS];

	struct {
		unsigned char *data;
		size_t len;
	} write;

	struct lws *wsi;
	struct avl_tree stubs;
};

struct lws_protocols wsubus_proto = {
	WSUBUS_PROTO_NAME,
	wsubus_cb,
	sizeof (struct remote_ubus),
	655360, // arbitrary length
	0,    // - id
	NULL, // - user pointer
};

static char *make_jsonrpc_ubus_call(int id, const char *sid, const char *obj, const char *method, json_object *arg)
{
#if 0
	json_object *rpc = json_object_new_object();
	json_object_object_add(rpc, "jsonrpc", json_object_new_string("2.0"));
	json_object_object_add(rpc, "id", json_object_new_int(id));
	json_object_object_add(rpc, "method", json_object_new_string("call"));
	json_object *params = json_object_new_array();
	json_object_array_add(params, json_object_new_string(sid));
	json_object_object_add(rpc, "params", params);
#endif
	static char buf[2048];
	snprintf(buf, sizeof buf, "{"
			"\"jsonrpc\":\"2.0\",\"id\":%d,"
			"\"method\":\"call\","
			"\"params\":[\"%s\", \"%s\", \"%s\", %s]"
			"}",
			id,
			sid ? sid : "00000000000000000000000000000000",
			obj, method, arg ? json_object_to_json_string(arg) : "{}");
	return buf;
}

static char *make_jsonrpc_ubus_list(int id, const char *sid, const char *pattern)
{
	static char buf[2048];
	snprintf(buf, sizeof buf, "{"
			"\"jsonrpc\":\"2.0\",\"id\":%d,"
			"\"method\":\"list\","
			"\"params\":[\"%s\", \"%s\"]"
			"}",
			id,
			sid ? sid : "00000000000000000000000000000000",
			pattern);
	return buf;
}

static char *make_jsonrpc_ubus_listen(int id, const char *sid, const char *pattern)
{
	static char buf[2048];
	snprintf(buf, sizeof buf, "{"
			"\"jsonrpc\":\"2.0\",\"id\":%d,"
			"\"method\":\"subscribe\","
			"\"params\":[\"%s\", \"%s\"]"
			"}",
			id,
			sid ? sid : "00000000000000000000000000000000",
			pattern);
	return buf;
}

struct remote_stub {
	struct remote_ubus *remote;

	struct avl_node avl;

	struct blobmsg_policy *method_args;

	struct ubus_object obj;
	struct ubus_object_type obj_type;
	struct ubus_method methods[0];
};

int remote_stub_handle_call(struct ubus_context *ubus_ctx, struct ubus_object *obj, struct ubus_request_data *req,
		const char *method, struct blob_attr *args)
{
	lwsl_notice("stub %s %s called\n", obj->name, method);
	lwsl_notice("obj name %s , path %s , type name %s\n", obj->name, obj->path, obj->type->name);

	struct remote_stub *stub = container_of(obj, struct remote_stub, obj);

	unsigned call_idx = find_first_zero(stub->remote->waiting_for.call);

	if (!call_idx || call_idx > MAX_INFLIGHT_CALLS) {
		lwsl_err("no space, too many calls being proxied, max %d\n", MAX_INFLIGHT_CALLS);
		return UBUS_STATUS_NOT_SUPPORTED;
	}

	char *args_json = blobmsg_format_json(args, false);
	json_object *args_jobj = args_json ? json_tokener_parse(args_json) : NULL;

	char *local_name = strchr(obj->name, '/')+1;
	lwsl_notice("will call %s obj on ...\n", local_name);

	// TODO save req + id somewhere so we can respond
	// or reuse seq as id and to it that way?

	char *d = make_jsonrpc_ubus_call(++stub->remote->call_id, stub->remote->sid, local_name, method, args_jobj);

	free(args_json);

	if (stub->remote->write.data) {
		lwsl_err("writing in progress, can't proxy call\n");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	--call_idx;
	stub->remote->calls[call_idx].jsonrpc_id = stub->remote->call_id;
	ubus_defer_request(ubus_ctx, req, &stub->remote->calls[call_idx].ureq);
	stub->remote->waiting_for.call |= (1U << call_idx);

	stub->remote->write.data = (unsigned char*)d;
	stub->remote->write.len = strlen(d);
	lws_callback_on_writable(stub->remote->wsi);

	return 0;
}

struct remote_stub* remote_stub_create(struct remote_ubus *remote, const char *object, json_object *signature)
{
	size_t num_methods = json_object_object_length(signature);
	size_t num_args = 0;
	{
		json_object_object_foreach(signature, mname, margs) {
			num_args += json_object_object_length(margs);
			(void)mname;
		}
	}

	struct remote_stub *stub = calloc(1, sizeof *stub + num_methods * sizeof stub->methods[0]);
	stub->method_args = calloc(num_args, sizeof stub->method_args[0]);
	stub->remote = remote;

	stub->obj.type = &stub->obj_type;
	stub->obj_type.n_methods = num_methods;
	stub->obj_type.methods = stub->methods;

	struct ubus_method *m = stub->methods;
	struct blobmsg_policy *b = stub->method_args;

	json_object_object_foreach(signature, mname, margs) {
		m->name = strdup(mname);
		m->n_policy = json_object_object_length(margs);
		m->policy = b;
		m->handler = remote_stub_handle_call;

		json_object_object_foreach(margs, aname, atype) {
			char c = json_object_get_string(atype)[0];
			b->type = (
					c == 'a' ? BLOBMSG_TYPE_ARRAY  :
					c == 'o' ? BLOBMSG_TYPE_TABLE  :
					c == 's' ? BLOBMSG_TYPE_STRING :
					c == 'n' ? BLOBMSG_TYPE_INT32  :
					c == 'b' ? BLOBMSG_TYPE_INT8   : BLOBMSG_TYPE_UNSPEC);
			b->name = strdup(aname);
			++b;
		}
		++m;
	};

	size_t objname_sz = strlen(object) + INET6_ADDRSTRLEN + 2;
	char *objname = malloc(objname_sz);

	lws_get_peer_simple(remote->wsi, objname, objname_sz);
	strcat(objname, "/");
	strcat(objname, object);

	stub->obj_type.name = objname;

	stub->obj.name = objname;
	stub->obj.type = &stub->obj_type;
	stub->obj.n_methods = stub->obj_type.n_methods;
	stub->obj.methods = stub->obj_type.methods;

	stub->avl.key = strchr(objname, '/')+1;
	avl_insert(&remote->stubs, &stub->avl);

	struct prog_context *global = lws_context_user(lws_get_context(stub->remote->wsi));
	ubus_add_object(&global->ubus_ctx, &stub->obj);

	return stub;
}

void remote_stub_destroy(struct remote_stub *stub)
{
	struct prog_context *global = lws_context_user(lws_get_context(stub->remote->wsi));
	ubus_remove_object(&global->ubus_ctx, &stub->obj);

	avl_delete(&stub->remote->stubs, &stub->avl);
	free((char*)stub->obj_type.name);
	free(stub->method_args);
	free(stub);
}

static int wsubus_cb(struct lws *wsi,
		enum lws_callback_reasons reason,
		void *user,
		void *in,
		size_t len)
{
	struct remote_ubus *remote = user;

	struct prog_context *global = lws_context_user(lws_get_context(wsi));

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		if (ubus_connect_ctx(&global->ubus_ctx, global->ubus_path)) {
			lwsl_err("failed to connect to ubus\n");
			return -1;
		}
		ubus_add_uloop(&global->ubus_ctx);
		return 0;

	case LWS_CALLBACK_CLIENT_ESTABLISHED: {
		remote->wsi = wsi;
		memset(&remote->waiting_for, 0, sizeof remote->waiting_for);
		avl_init(&remote->stubs, avl_strcmp, false, NULL);

		remote->waiting_for.login = 1;

		json_object *adminadmin = json_object_new_object();
		json_object_object_add(adminadmin, "username", json_object_new_string("admin"));
		json_object_object_add(adminadmin, "password", json_object_new_string("admin"));

		char *d = make_jsonrpc_ubus_call(++remote->call_id, NULL, "session", "login", adminadmin);
		remote->write.data = (unsigned char*)d;
		remote->write.len = strlen(d);

		json_object_put(adminadmin);
		lws_callback_on_writable(wsi);

		return 0;
	}

	case LWS_CALLBACK_CLOSED:
		return 0;

	case LWS_CALLBACK_CLIENT_RECEIVE: {
		struct json_tokener *jtok = json_tokener_new();
		struct json_object *jobj = json_tokener_parse_ex(jtok, in, len);

		lwsl_notice("received, len %d < %.*s > \n", len, len > 200 ? 200 : len, in);

		if (!jobj)
			goto out;

		struct json_object *tmp;
		if (json_object_object_get_ex(jobj, "result", &tmp)) {
			// result came back
			if (remote->waiting_for.login) {
				if (
						json_object_is_type(tmp, json_type_array)
						&& !json_object_get_int(json_object_array_get_idx(tmp, 0))
						&& (tmp = json_object_array_get_idx(tmp, 1))
						&& json_object_object_get_ex(tmp, "ubus_rpc_session", &tmp)
						&& json_object_is_type(tmp, json_type_string)
				   ) {
					remote->waiting_for.login = 0;
					strcpy(remote->sid, json_object_get_string(tmp));
				} else {
					// TODO
					lwsl_err("response to login not valid\n");
					goto out;
				}

				char *d = make_jsonrpc_ubus_listen(++remote->call_id, remote->sid, "*");
				remote->write.data = (unsigned char*)d;
				remote->write.len = strlen(d);
				remote->waiting_for.listen = 1;
				lws_callback_on_writable(wsi);
			} else if (remote->waiting_for.listen) {
				if (
						json_object_is_type(tmp, json_type_array)
						&& !json_object_get_int(json_object_array_get_idx(tmp, 0))) {
					remote->waiting_for.listen = 0;
				} else {
					// TODO
					lwsl_err("response to ubus listen not valid\n");
					goto out;
				}

				char *d = make_jsonrpc_ubus_list(++remote->call_id, remote->sid, "*");
				remote->write.data = (unsigned char*)d;
				remote->write.len = strlen(d);
				remote->waiting_for.list = 1;
				lws_callback_on_writable(wsi);
				break;
			} else if (remote->waiting_for.list) {
				if (
						json_object_is_type(tmp, json_type_array)
						&& !json_object_get_int(json_object_array_get_idx(tmp, 0))
						&& (tmp = json_object_array_get_idx(tmp, 1))
						&& json_object_is_type(tmp, json_type_object)) {
					json_object_object_foreach(tmp, obj_name, obj_methods) {
						remote_stub_create(remote, obj_name, obj_methods);
					}
				}

				// FIXME when multiple object add events fire, only first one will be handled
				remote->waiting_for.list = 0;
			} else if (remote->waiting_for.call) {
				json_object *id_jobj, *rc_jobj;
				int id, call_idx = -1;
				if (
						json_object_object_get_ex(jobj, "id", &id_jobj)
						&& json_object_is_type(id_jobj, json_type_int)
						&& (id = json_object_get_int(id_jobj), 1) ) {
					lwsl_notice("got response to call %d \n", id);
					int call_bit;
					while ((call_bit = (remote->waiting_for.call & -remote->waiting_for.call))) {
						int idx = __builtin_ctz(call_bit);
						if (remote->calls[idx].jsonrpc_id == id) {
							call_idx = idx;
							remote->waiting_for.call &= ~call_bit;
							break;
						}
					}
				}

				if (call_idx < 0) {
					lwsl_err("call id not found, ignoring\n");
					return 0;
				}

				struct prog_context *global = lws_context_user(lws_get_context(remote->wsi));
				// will send response to found request

				rc_jobj = json_object_array_get_idx(tmp, 0);
				if (json_object_is_type(rc_jobj, json_type_int)) {
					if ((tmp = json_object_array_get_idx(tmp, 1)) && json_object_is_type(tmp, json_type_object))  {
						struct blob_buf b = {};
						blob_buf_init(&b, 0);
						blobmsg_add_object(&b, tmp);

						ubus_send_reply(&global->ubus_ctx, &remote->calls[call_idx].ureq, b.head);

						blob_buf_free(&b);
					}

					ubus_complete_deferred_request(&global->ubus_ctx, &remote->calls[call_idx].ureq, json_object_get_int(rc_jobj));
				} else {
					ubus_complete_deferred_request(&global->ubus_ctx, &remote->calls[call_idx].ureq, UBUS_STATUS_UNKNOWN_ERROR);
				}
			}
		} else if (json_object_object_get_ex(jobj, "method", &tmp)) {
			json_object *t;
			if (
					!strcmp("event", json_object_get_string(tmp))
					&& json_object_object_get_ex(jobj, "params", &tmp)
					&& json_object_is_type(tmp, json_type_object)
					&& json_object_object_get_ex(tmp, "type", &t)
					&& json_object_is_type(t, json_type_string)
					&& json_object_object_get_ex(tmp, "data", &tmp)) {
				// object add/remove event
				if (
						!strcmp("ubus.object.add", json_object_get_string(t))
						&& json_object_object_get_ex(tmp, "path", &tmp)
						&& json_object_is_type(tmp, json_type_string)) {
					// object added, look it up, when done

#if 0
					char *d = make_jsonrpc_ubus_list(++remote->call_id, remote->sid, json_object_get_string(tmp));
					remote->write.data = (unsigned char*)d;
					remote->write.len = strlen(d);
					remote->waiting_for = W8ing4_LIST;
					lws_callback_on_writable(wsi);
#endif
					// FIXME: above should be used, but below is workaround because we can't wait for multiple lists
					if (!remote->waiting_for.list) {
						char *d = make_jsonrpc_ubus_list(++remote->call_id, remote->sid, "*");
						remote->write.data = (unsigned char*)d;
						remote->write.len = strlen(d);
						remote->waiting_for.list = 1;
						lws_callback_on_writable(wsi);
					}
				} else if (
						!strcmp("ubus.object.remove", json_object_get_string(t))
						&& json_object_object_get_ex(tmp, "path", &tmp)
						&& json_object_is_type(tmp, json_type_string)) {
					//remote_stub_destroy(remote, json_object_get_string(tmp));
				}
			}
		}

out:
		if (jobj)
			json_object_put(jobj);

		json_tokener_free(jtok);
		
		return 0;
	}

	case LWS_CALLBACK_CLIENT_WRITEABLE: {
		if (remote->write.data) {
			lwsl_notice("sending, len %d < %.*s> \n", remote->write.len, remote->write.len, remote->write.data);
			int ret = (int)remote->write.len != lws_write(wsi, remote->write.data, remote->write.len, LWS_WRITE_TEXT);
			remote->write.data = NULL;
			remote->write.len = 0;
			return ret;
		} else {
			return -1;
		}
	}

	default:
		return 0;
	}
	return 0;
}

