#include "ble_pairing_policy.h"

#include <stddef.h>
#include <stdio.h>

ble_pairing_mode_t ble_pairing_mode_resolve(bool display_cb_registered, bool static_passkey_set) {
    if (display_cb_registered) {
        return BLE_PAIRING_DISPLAY_PASSKEY;
    }
    if (static_passkey_set) {
        return BLE_PAIRING_STATIC_PASSKEY;
    }
    return BLE_PAIRING_JUST_WORKS;
}

bool ble_pairing_passkey_parse(const char* s, uint32_t* out) {
    if (s == NULL || out == NULL) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    if (s[6] != '\0') {
        return false;
    }
    *out = v;
    return true;
}

bool ble_pairing_passkey_valid(const char* s) {
    uint32_t scratch;
    return ble_pairing_passkey_parse(s, &scratch);
}

uint32_t ble_pairing_backoff_ms(unsigned consecutive_failures) {
    if (consecutive_failures == 0) {
        return 0;
    }
    if (consecutive_failures > 6) {
        return 60000u;
    }
    return 1000u << (consecutive_failures - 1);
}

const char* ble_pairing_mode_name(ble_pairing_mode_t m) {
    switch (m) {
    case BLE_PAIRING_STATIC_PASSKEY:
        return "static-passkey";
    case BLE_PAIRING_DISPLAY_PASSKEY:
        return "passkey-display";
    case BLE_PAIRING_JUST_WORKS:
    default:
        return "just-works";
    }
}

#define BLE_PAIRING_PENDING_VALID_BIT (1u << 31)
#define BLE_PAIRING_PENDING_SHOW_BIT (1u << 30)
#define BLE_PAIRING_PENDING_CODE_MASK 0x000FFFFFu /* passkey is 0..999999, fits in 20 bits */

uint32_t ble_pairing_pending_pack(uint32_t passkey, bool show) {
    uint32_t word = BLE_PAIRING_PENDING_VALID_BIT | (passkey & BLE_PAIRING_PENDING_CODE_MASK);
    if (show) {
        word |= BLE_PAIRING_PENDING_SHOW_BIT;
    }
    return word;
}

bool ble_pairing_pending_unpack(uint32_t word, uint32_t* out_passkey, bool* out_show) {
    if ((word & BLE_PAIRING_PENDING_VALID_BIT) == 0) {
        return false;
    }
    if (out_passkey != NULL) {
        *out_passkey = word & BLE_PAIRING_PENDING_CODE_MASK;
    }
    if (out_show != NULL) {
        *out_show = (word & BLE_PAIRING_PENDING_SHOW_BIT) != 0;
    }
    return true;
}

void ble_pairing_format_code(uint32_t code, char* out, size_t out_len) {
    if (out == NULL || out_len == 0) {
        return;
    }
    /* Formatting behind a runtime-sized destination also keeps callers clear
     * of -Wformat-truncation: the checker does not track that code/1000u and
     * code%1000u are both under 1000, so it scores each %03u as a full
     * unsigned int and rejects the 8-byte buffer the output actually needs.
     * It cannot make that judgement about a pointer parameter. */
    snprintf(out, out_len, "%03u %03u", (unsigned)(code / 1000u), (unsigned)(code % 1000u));
}
