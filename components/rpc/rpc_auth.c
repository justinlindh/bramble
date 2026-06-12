#include "rpc_auth.h"

#include <string.h>

/*
 * Unauthenticated allowlist. Keep this list tiny and boring: methods here
 * are reachable by anyone who can open a WS or BLE connection to the
 * device, with no token. Identification only; no mesh state, no telemetry,
 * no mutation. Anything else requires the device token.
 *
 *   bramble.ping        liveness check for connection UIs
 *   bramble.getVersion  firmware/protocol/hardware identification so a
 *                       pairing UI can say "this is a Bramble device,
 *                       firmware X on hardware Y, token required"
 */
static const char* const k_unauth_allowlist[] = {
    "bramble.ping",
    "bramble.getVersion",
};

bool rpc_auth_token_len_ok(size_t len) { return len == 0 || len >= RPC_AUTH_TOKEN_MIN_LEN; }

bool rpc_auth_notify_allowed(bool conn_authenticated, bool auth_disabled) {
    return conn_authenticated || auth_disabled;
}

int rpc_auth_notify_filter(const rpc_notify_client_t* clients, int count, bool auth_disabled,
                           int* out_fds, int max_out) {
    int n = 0;
    if (!clients || !out_fds) {
        return 0;
    }
    for (int i = 0; i < count && n < max_out; i++) {
        if (rpc_auth_notify_allowed(clients[i].authenticated, auth_disabled)) {
            out_fds[n++] = clients[i].fd;
        }
    }
    return n;
}

bool rpc_auth_method_allowed(const char* method, bool authenticated) {
    if (!method) {
        return false;
    }
    if (authenticated) {
        return true;
    }
    for (size_t i = 0; i < sizeof(k_unauth_allowlist) / sizeof(k_unauth_allowlist[0]); i++) {
        if (strcmp(method, k_unauth_allowlist[i]) == 0) {
            return true;
        }
    }
    return false;
}
