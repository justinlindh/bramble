#include "ble_pairing_policy.h"

#include <stddef.h>

ble_pairing_mode_t ble_pairing_mode_resolve(bool display_cb_registered, bool static_passkey_set) {
    if (display_cb_registered) {
        return BLE_PAIRING_DISPLAY_PASSKEY;
    }
    if (static_passkey_set) {
        return BLE_PAIRING_STATIC_PASSKEY;
    }
    return BLE_PAIRING_JUST_WORKS;
}

bool ble_pairing_passkey_valid(const char* s) {
    if (s == NULL) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return s[6] == '\0';
}

bool ble_pairing_passkey_parse(const char* s, uint32_t* out) {
    if (out == NULL || !ble_pairing_passkey_valid(s)) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return true;
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
