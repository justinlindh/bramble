#ifndef BRAMBLE_BLE_REDACT_H
#define BRAMBLE_BLE_REDACT_H

#include <stdbool.h>

/*
 * The first pre-auth BLE write is the bare device auth token (f1aa770d
 * handshake design). Echoing inbound BLE payloads before authentication prints
 * that secret in cleartext (NEW-SEC-5). This predicate gates whether a received
 * RPC line body may be logged at all: never before auth. Callers additionally
 * demote post-auth body logs to DEBUG, since authenticated bodies can still
 * carry secrets (e.g. bramble.setAuthToken params).
 */
static inline bool ble_rpc_body_loggable(bool authenticated) {
    return authenticated;
}

#endif /* BRAMBLE_BLE_REDACT_H */
