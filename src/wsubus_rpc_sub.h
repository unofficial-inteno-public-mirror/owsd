/*
 * ubus over websocket - ubus event subscription
 */

#include <json-c/json.h>
#include <libubox/uloop.h>
#include <libubus.h>

// TODO maybe these will be same type
struct ubusrpc_blob_sub {
	struct blob_attr *src_blob;

	const char *sid;
	const char *object;
};

struct ubusrpc_blob_unsub_by_id {
	struct blob_attr *src_blob;

	const char *sid;
	uint32_t id;
};

struct ubusrpc_blob;

struct lws;

void wsubus_unsubscribe_all();
int wsubus_unsubscribe_by_id(uint32_t id);
int wsubus_unsubscribe_by_sid_pattern(const char *sid, const char *pattern);
int wsubus_unsubscribe_all_by_sid(const char *sid);

int ubusrpc_blob_sub_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob);
int ubusrpc_blob_sub_list_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob);
int ubusrpc_blob_unsub_by_id_parse(struct ubusrpc_blob *ubusrpc, struct blob_attr *blob);
int ubusrpc_handle_sub(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id);
int ubusrpc_handle_sub_list(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id);
int ubusrpc_handle_unsub_by_id(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id);
