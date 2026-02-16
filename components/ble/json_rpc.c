#include "json_rpc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Minimal JSON helpers — no external deps

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

// Extract a quoted string value starting after the opening quote.
// Returns pointer past closing quote, or NULL on error.
static const char *extract_string(const char *p, const char *end, char *out, size_t out_len) {
    size_t i = 0;
    while (p < end && *p != '"') {
        if (i < out_len - 1)
            out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    if (p >= end) return NULL;
    return p + 1; // skip closing quote
}

// Find a top-level string key in JSON object, return pointer to value start.
// json should point past the opening '{'.
static const char *find_key(const char *json, const char *end, const char *key) {
    const char *p = json;
    size_t key_len = strlen(key);

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) return NULL;

        if (*p == '}') return NULL;
        if (*p == ',') { p++; continue; }

        // Expect a quoted key
        if (*p != '"') return NULL;
        p++;
        const char *ks = p;
        while (p < end && *p != '"') p++;
        if (p >= end) return NULL;
        size_t kl = (size_t)(p - ks);
        p++; // past closing quote

        p = skip_ws(p, end);
        if (p >= end || *p != ':') return NULL;
        p++;
        p = skip_ws(p, end);
        if (p >= end) return NULL;

        if (kl == key_len && memcmp(ks, key, key_len) == 0)
            return p;

        // Skip the value
        if (*p == '"') {
            p++;
            while (p < end && *p != '"') p++;
            if (p < end) p++;
        } else if (*p == '{') {
            int depth = 1;
            p++;
            while (p < end && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                p++;
            }
        } else if (*p == '[') {
            int depth = 1;
            p++;
            while (p < end && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
        } else {
            // number, bool, null
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\n')
                p++;
        }
    }
    return NULL;
}

rpc_method_t rpc_method_from_string(const char *method) {
    if (strcmp(method, "get_config") == 0)    return RPC_METHOD_GET_CONFIG;
    if (strcmp(method, "set_config") == 0)    return RPC_METHOD_SET_CONFIG;
    if (strcmp(method, "send_message") == 0)  return RPC_METHOD_SEND_MESSAGE;
    if (strcmp(method, "get_nodes") == 0)     return RPC_METHOD_GET_NODES;
    if (strcmp(method, "get_routes") == 0)    return RPC_METHOD_GET_ROUTES;
    if (strcmp(method, "get_status") == 0)    return RPC_METHOD_GET_STATUS;
    if (strcmp(method, "get_messages") == 0)  return RPC_METHOD_GET_MESSAGES;
    return RPC_METHOD_UNKNOWN;
}

int rpc_parse_request(const char *json, size_t len, rpc_request_t *req) {
    memset(req, 0, sizeof(*req));
    if (!json || len == 0) return -1;

    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '{') return -1;
    p++; // past '{'

    // Find method
    const char *val = find_key(p, end, "method");
    if (!val || *val != '"') return -1;
    val++;
    val = extract_string(val, end, req->method_str, sizeof(req->method_str));
    if (!val) return -1;
    if (req->method_str[0] == '\0') return -1;

    req->method = rpc_method_from_string(req->method_str);

    // Find id
    const char *id_val = find_key(p, end, "id");
    if (id_val) {
        req->id = (int)strtol(id_val, NULL, 10);
    }

    // Find params (object only)
    const char *params_val = find_key(p, end, "params");
    if (params_val && *params_val == '{') {
        const char *pp = params_val + 1;
        while (pp < end && req->param_count < JSON_RPC_MAX_PARAMS) {
            pp = skip_ws(pp, end);
            if (pp >= end) break;
            if (*pp == '}') break;
            if (*pp == ',') { pp++; continue; }
            if (*pp != '"') break;
            pp++;
            pp = extract_string(pp, end, req->params[req->param_count].key,
                                sizeof(req->params[0].key));
            if (!pp) break;
            pp = skip_ws(pp, end);
            if (pp >= end || *pp != ':') break;
            pp++;
            pp = skip_ws(pp, end);
            if (pp >= end) break;
            if (*pp == '"') {
                pp++;
                pp = extract_string(pp, end, req->params[req->param_count].value,
                                    sizeof(req->params[0].value));
                if (!pp) break;
            } else {
                // numeric/bool value
                size_t i = 0;
                while (pp < end && *pp != ',' && *pp != '}' && *pp != ' ' &&
                       i < sizeof(req->params[0].value) - 1) {
                    req->params[req->param_count].value[i++] = *pp++;
                }
                req->params[req->param_count].value[i] = '\0';
            }
            req->param_count++;
        }
    }

    req->valid = true;
    return 0;
}

int rpc_build_response(const rpc_response_t *resp, char *buf, size_t buf_len) {
    if (resp->is_error) {
        return snprintf(buf, buf_len,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%d}",
            resp->error_code, resp->error_message, resp->id);
    }
    return snprintf(buf, buf_len,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%d}",
        resp->result, resp->id);
}

int rpc_build_error(int id, int code, const char *message, char *buf, size_t buf_len) {
    return snprintf(buf, buf_len,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%d}",
        code, message, id);
}
