#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct cJSON cJSON;

/* JSON-RPC 2.0 standard error codes */
#define RPC_ERR_PARSE (-32700)
#define RPC_ERR_INVALID_REQ (-32600)
#define RPC_ERR_NOT_FOUND (-32601)
#define RPC_ERR_INVALID_PARAMS (-32602)
#define RPC_ERR_INTERNAL (-32603)

/* Application error codes */
#define RPC_ERR_RADIO (-1001)
#define RPC_ERR_CHANNEL (-1002)
#define RPC_ERR_RATE_LIMIT (-1003)
#define RPC_ERR_NOT_SUPPORTED (-1004)
#define RPC_ERR_UNAUTHORIZED (-1005)

/**
 * RPC method handler. Receives params object, populates result object.
 * Returns 0 on success, negative error code on failure.
 */
typedef int (*rpc_method_handler_t)(const cJSON* params, cJSON* result);

/**
 * Notification transport callback. Called with serialized JSON notification.
 */
typedef void (*rpc_notify_cb_t)(const char* json, size_t len, void* ctx);

/** Initialize dispatcher, reset method table. */
void rpc_init(void);

/** Register a named method handler. Returns 0 or -1 if table full. */
int rpc_register(const char* method, rpc_method_handler_t handler);

/**
 * Parse JSON-RPC 2.0 request, dispatch to handler, write response.
 * Returns length of response written to json_out, or -1 on error.
 *
 * Full-privilege dispatch. Use only for callers that are authenticated or
 * trusted by design (the serial CLI: physical access is the pairing
 * bootstrap, see docs/SECURITY-MODEL.md). Network transports must use
 * rpc_dispatch_authed() with the connection's auth state.
 */
int rpc_dispatch(const char* json_in, char* json_out, size_t out_len);

/**
 * Same as rpc_dispatch(), but when `authenticated` is false only the
 * unauthenticated allowlist (rpc_auth.h) is dispatchable; everything else
 * gets an RPC_ERR_UNAUTHORIZED error response without the method table
 * being consulted.
 */
int rpc_dispatch_authed(const char* json_in, char* json_out, size_t out_len, bool authenticated);

/** Send a JSON-RPC notification to all registered transports. */
void rpc_notify(const char* method, const cJSON* params);

/** Register a transport for outgoing notifications. Returns 0 or -1 if full. */
int rpc_register_notify_transport(rpc_notify_cb_t cb, void* ctx);
