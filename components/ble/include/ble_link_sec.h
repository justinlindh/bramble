#ifndef BRAMBLE_BLE_LINK_SEC_H
#define BRAMBLE_BLE_LINK_SEC_H

#include <stdbool.h>

/*
 * BLE link-layer security gate (issue #73).
 *
 * The first pre-auth write on the NUS TX characteristic is the bare RPC auth
 * token, and every subsequent write is a JSON-RPC request against the full
 * method surface. Without link encryption a passive sniffer in range during a
 * legitimate app session captures the token and inherits that surface.
 *
 * The GATT characteristics therefore carry the _ENC flags, and the firmware
 * additionally re-checks the connection's security state in the access
 * callback before touching a payload. Belt and braces: the flags are enforced
 * by the NimBLE ATT server, this predicate is enforced by us, and the two
 * disagreeing is a bug we would rather fail closed on.
 *
 * The predicate is deliberately trivial and deliberately has no "allow when
 * unencrypted" escape hatch: there is no configuration, no board profile, and
 * no development mode in which cleartext RPC over BLE is acceptable. It lives
 * in a header of its own so the fail-closed behaviour is host-testable without
 * an ESP-IDF build (same pattern as ble_redact.h).
 */
static inline bool ble_link_payload_permitted(bool link_encrypted) { return link_encrypted; }

#endif /* BRAMBLE_BLE_LINK_SEC_H */
