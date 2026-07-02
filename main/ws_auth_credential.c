#include "ws_auth_credential.h"
#include <string.h>

#define BEARER_PREFIX "Bearer "
#define SUBPROTO_AUTH_PREFIX "bramble.v1.auth."

static bool copy_token(const char* start, size_t n, char* out, size_t out_len) {
    if (n == 0 || n + 1 > out_len) {
        return false;
    }
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

bool ws_auth_extract_token(const char* authorization, const char* subprotocols,
                           char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    /* 1. Authorization: Bearer <token> (non-browser clients). */
    if (authorization && strncmp(authorization, BEARER_PREFIX, strlen(BEARER_PREFIX)) == 0) {
        const char* tok = authorization + strlen(BEARER_PREFIX);
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        size_t n = strlen(tok);
        while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '\t')) {
            n--;
        }
        return copy_token(tok, n, out, out_len);
    }

    /* 2. Sec-WebSocket-Protocol: bramble.v1.auth.<token>[, bramble.v1] */
    if (subprotocols) {
        const char* p = subprotocols;
        size_t plen = strlen(SUBPROTO_AUTH_PREFIX);
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') {
                p++;
            }
            const char* entry = p;
            while (*p && *p != ',') {
                p++;
            }
            size_t entry_len = (size_t)(p - entry);
            while (entry_len > 0 &&
                   (entry[entry_len - 1] == ' ' || entry[entry_len - 1] == '\t')) {
                entry_len--;
            }
            if (entry_len > plen && strncmp(entry, SUBPROTO_AUTH_PREFIX, plen) == 0) {
                return copy_token(entry + plen, entry_len - plen, out, out_len);
            }
        }
    }

    return false;
}
