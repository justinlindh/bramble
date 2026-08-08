#ifndef BRAMBLE_BLE_PAIRING_POLICY_H
#define BRAMBLE_BLE_PAIRING_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * BLE SMP pairing policy (spec: docs/superpowers/specs/
 * 2026-08-07-ble-pairing-codes-design.md).
 *
 * Pure logic, host-testable without an ESP-IDF build (same pattern as
 * ble_link_sec.h). ble_server.c maps the mode onto NimBLE sm_io_cap and
 * sm_mitm; this module never includes NimBLE headers.
 *
 * Mode table:
 *   display cb registered            -> DISPLAY_PASSKEY (random per pairing)
 *   no display cb, static key set    -> STATIC_PASSKEY  (operator-set code)
 *   neither                          -> JUST_WORKS      (bootstrap; RPC token
 *                                       remains the compensating control)
 */

typedef enum {
    BLE_PAIRING_JUST_WORKS = 0,
    BLE_PAIRING_STATIC_PASSKEY,
    BLE_PAIRING_DISPLAY_PASSKEY,
} ble_pairing_mode_t;

ble_pairing_mode_t ble_pairing_mode_resolve(bool display_cb_registered, bool static_passkey_set);

/* Exactly 6 ASCII digits; leading zeros significant. */
bool ble_pairing_passkey_valid(const char* s);

/* Valid string -> numeric passkey 0..999999. False on invalid input. */
bool ble_pairing_passkey_parse(const char* s, uint32_t* out);

/* Advertising-restart delay after N consecutive failed pairing attempts:
 * 0 for N=0, then 1s doubling per failure, capped at 60s. Mitigates the
 * passkey-entry bit-leak an active MITM gets from repeated attempts. */
uint32_t ble_pairing_backoff_ms(unsigned consecutive_failures);

/* Wire name used by bramble.getBleSecurity. */
const char* ble_pairing_mode_name(ble_pairing_mode_t m);

#endif /* BRAMBLE_BLE_PAIRING_POLICY_H */
