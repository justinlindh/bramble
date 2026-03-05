#include "rpc_dispatcher.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG = "rpc";

#define MAX_NOTIFY_TRANSPORTS 4

typedef struct {
    const char* method;
    rpc_method_handler_t handler;
} rpc_entry_t;

typedef struct {
    rpc_notify_cb_t cb;
    void* ctx;
} notify_transport_t;

static rpc_entry_t s_methods[CONFIG_BRAMBLE_RPC_MAX_METHODS];
static int s_method_count;

static notify_transport_t s_transports[MAX_NOTIFY_TRANSPORTS];
static int s_transport_count;

void rpc_init(void) {
    memset(s_methods, 0, sizeof(s_methods));
    s_method_count = 0;
    memset(s_transports, 0, sizeof(s_transports));
    s_transport_count = 0;
    ESP_LOGI(TAG, "RPC dispatcher initialized");
}

int rpc_register(const char* method, rpc_method_handler_t handler) {
    if (s_method_count >= CONFIG_BRAMBLE_RPC_MAX_METHODS) {
        ESP_LOGE(TAG, "Method table full, cannot register '%s'", method);
        return -1;
    }
    s_methods[s_method_count].method = method;
    s_methods[s_method_count].handler = handler;
    s_method_count++;
    ESP_LOGI(TAG, "Registered method '%s' (%d/%d)", method, s_method_count,
             CONFIG_BRAMBLE_RPC_MAX_METHODS);
    return 0;
}

int rpc_register_notify_transport(rpc_notify_cb_t cb, void* ctx) {
    if (s_transport_count >= MAX_NOTIFY_TRANSPORTS) {
        ESP_LOGE(TAG, "Notify transport table full");
        return -1;
    }
    s_transports[s_transport_count].cb = cb;
    s_transports[s_transport_count].ctx = ctx;
    s_transport_count++;
    return 0;
}

static int format_error(cJSON* id, int code, const char* message, char* json_out, size_t out_len) {
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");

    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);

    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    char* out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!out)
        return -1;

    size_t len = strlen(out);
    if (len >= out_len) {
        free(out);
        return -1;
    }
    memcpy(json_out, out, len + 1);
    free(out);
    return (int)len;
}

static __attribute__((unused)) int format_error_with_details(cJSON* id, int code,
                                                             const char* message,
                                                             const char* details, char* json_out,
                                                             size_t out_len) {
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");

    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    if (details && details[0] != '\0') {
        cJSON_AddStringToObject(err, "details", details);
    }
    cJSON_AddItemToObject(resp, "error", err);

    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    char* out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!out)
        return -1;

    size_t len = strlen(out);
    if (len >= out_len) {
        free(out);
        return -1;
    }
    memcpy(json_out, out, len + 1);
    free(out);
    return (int)len;
}

static const char* error_message_for_code(int code) {
    switch (code) {
    case RPC_ERR_PARSE:
        return "Parse error";
    case RPC_ERR_INVALID_REQ:
        return "Invalid Request";
    case RPC_ERR_NOT_FOUND:
        return "Method not found";
    case RPC_ERR_INVALID_PARAMS:
        return "Invalid params";
    case RPC_ERR_INTERNAL:
        return "Internal error";
    case RPC_ERR_RADIO:
        return "Radio error";
    case RPC_ERR_CHANNEL:
        return "Channel error";
    case RPC_ERR_RATE_LIMIT:
        return "Rate limited";
    case RPC_ERR_NOT_SUPPORTED:
        return "Not supported";
    default:
        return "Error";
    }
}

int rpc_dispatch(const char* json_in, char* json_out, size_t out_len) {
    cJSON* req = cJSON_Parse(json_in);
    if (!req) {
        ESP_LOGW(TAG, "Failed to parse JSON-RPC request");
        return format_error(NULL, RPC_ERR_PARSE, "Parse error", json_out, out_len);
    }

    /* Validate jsonrpc field */
    cJSON* jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        ESP_LOGW(TAG, "Missing or invalid jsonrpc field");
        cJSON* id = cJSON_GetObjectItem(req, "id");
        int ret = format_error(id, RPC_ERR_INVALID_REQ, "Invalid Request", json_out, out_len);
        cJSON_Delete(req);
        return ret;
    }

    /* Validate method field */
    cJSON* method = cJSON_GetObjectItem(req, "method");
    if (!cJSON_IsString(method)) {
        ESP_LOGW(TAG, "Missing or invalid method field");
        cJSON* id = cJSON_GetObjectItem(req, "id");
        int ret = format_error(id, RPC_ERR_INVALID_REQ, "Invalid Request", json_out, out_len);
        cJSON_Delete(req);
        return ret;
    }

    /* Extract id */
    cJSON* id = cJSON_GetObjectItem(req, "id");

    /* Extract params */
    cJSON* params = cJSON_GetObjectItem(req, "params");
    cJSON* empty_params = NULL;
    if (!params) {
        empty_params = cJSON_CreateObject();
        params = empty_params;
    }

    /* Look up handler */
    rpc_method_handler_t handler = NULL;
    for (int i = 0; i < s_method_count; i++) {
        if (strcmp(s_methods[i].method, method->valuestring) == 0) {
            handler = s_methods[i].handler;
            break;
        }
    }

    if (!handler) {
        ESP_LOGW(TAG, "Method not found: '%s'", method->valuestring);
        int ret = format_error(id, RPC_ERR_NOT_FOUND, "Method not found", json_out, out_len);
        cJSON_Delete(empty_params);
        cJSON_Delete(req);
        return ret;
    }

    /* Call handler */
    cJSON* result = cJSON_CreateObject();
    int rc = handler(params, result);

    int ret;
    if (rc == 0) {
        /* Success response */
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(resp, "result", result);
        if (id) {
            cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
        } else {
            cJSON_AddNullToObject(resp, "id");
        }

        char* out = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);

        if (!out) {
            ret = -1;
        } else {
            size_t len = strlen(out);
            if (len >= out_len) {
                ESP_LOGW(TAG, "RPC response truncated for method '%s' (%zu >= %zu)",
                         method->valuestring, len, out_len);
                free(out);
                ret = -1;
            } else {
                memcpy(json_out, out, len + 1);
                free(out);
                ret = (int)len;
            }
        }
    } else {
        /* Error response */
        cJSON_Delete(result);
        ret = format_error(id, rc, error_message_for_code(rc), json_out, out_len);
    }

    cJSON_Delete(empty_params);
    cJSON_Delete(req);
    return ret;
}

void rpc_notify(const char* method, const cJSON* params) {
    cJSON* notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", method);
    if (params) {
        cJSON_AddItemToObject(notif, "params", cJSON_Duplicate(params, 1));
    }

    char* out = cJSON_PrintUnformatted(notif);
    cJSON_Delete(notif);
    if (!out)
        return;

    size_t len = strlen(out);
    for (int i = 0; i < s_transport_count; i++) {
        if (s_transports[i].cb) {
            s_transports[i].cb(out, len, s_transports[i].ctx);
        }
    }

    free(out);
}
