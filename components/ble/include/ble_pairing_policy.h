#ifndef BRAMBLE_BLE_PAIRING_POLICY_H
#define BRAMBLE_BLE_PAIRING_POLICY_H

#include <stdbool.h>
#include <stddef.h>
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

/*
 * Cross-task passkey-display handoff, shared by the two independent
 * producer/consumer pairs that need it: main.c (NimBLE host task ->
 * main loop task) and ui_pairing.c (NimBLE host task -> LVGL task). Both
 * pack passkey + show/valid into one word so a single atomic
 * store/exchange moves the whole request without tearing; see either
 * call site's cross-task comment for why a plain load/store is not
 * enough. This header only defines the bit layout and formatting; each
 * caller keeps and atomically accesses its own word.
 */
uint32_t ble_pairing_pending_pack(uint32_t passkey, bool show);

/* False (out params untouched) when word's valid bit is clear. */
bool ble_pairing_pending_unpack(uint32_t word, uint32_t* out_passkey, bool* out_show);

/* Formats code as "NNN NNN" (leading zeros kept) into out. out_len must be
 * at least 8 (6 digits + space + nul); longer buffers are fine. */
void ble_pairing_format_code(uint32_t code, char* out, size_t out_len);

#endif /* BRAMBLE_BLE_PAIRING_POLICY_H */
