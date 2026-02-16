#ifndef BRAMBLE_JSON_RPC_H
#define BRAMBLE_JSON_RPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JSON_RPC_MAX_METHOD_LEN 32
#define JSON_RPC_MAX_PARAMS 8
#define JSON_RPC_MAX_PARAM_LEN 64
#define JSON_RPC_MAX_RESPONSE_LEN 256

typedef enum {
    RPC_METHOD_UNKNOWN = 0,
    RPC_METHOD_GET_CONFIG,
    RPC_METHOD_SET_CONFIG,
    RPC_METHOD_SEND_MESSAGE,
    RPC_METHOD_GET_NODES,
    RPC_METHOD_GET_ROUTES,
    RPC_METHOD_GET_STATUS,
    RPC_METHOD_GET_MESSAGES,
} rpc_method_t;

typedef struct {
    char key[JSON_RPC_MAX_PARAM_LEN];
    char value[JSON_RPC_MAX_PARAM_LEN];
} rpc_param_t;

typedef struct {
    rpc_method_t method;
    char method_str[JSON_RPC_MAX_METHOD_LEN];
    rpc_param_t params[JSON_RPC_MAX_PARAMS];
    int param_count;
    int id;
    bool valid;
} rpc_request_t;

typedef struct {
    int id;
    bool is_error;
    int error_code;
    char error_message[64];
    char result[JSON_RPC_MAX_RESPONSE_LEN];
} rpc_response_t;

int rpc_parse_request(const char *json, size_t len, rpc_request_t *req);
int rpc_build_response(const rpc_response_t *resp, char *buf, size_t buf_len);
int rpc_build_error(int id, int code, const char *message, char *buf, size_t buf_len);
rpc_method_t rpc_method_from_string(const char *method);

#endif
