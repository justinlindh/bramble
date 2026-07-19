#include "wifi_ap_password.h"

#include "crypto.h"

#include <string.h>

static const char k_alphabet[] = WIFI_AP_PASSWORD_ALPHABET;

/* The 5-bits-per-character mapping below is only unbiased because the
 * alphabet is exactly 32 symbols. Catch an edited alphabet at compile time
 * rather than shipping a skewed password space. */
_Static_assert(sizeof(k_alphabet) - 1 == 32, "AP password alphabet must be exactly 32 symbols");
_Static_assert(WIFI_AP_PASSWORD_LEN >= WIFI_AP_PASSWORD_MIN,
               "derived AP password must satisfy the WPA2-PSK minimum");
_Static_assert(WIFI_AP_PASSWORD_LEN <= WIFI_AP_PASSWORD_MAX,
               "derived AP password must satisfy the WPA2-PSK maximum");

int wifi_ap_password_derive(const uint8_t* secret, size_t secret_len, char* out, size_t out_len) {
    if (!out || out_len < WIFI_AP_PASSWORD_BUFSZ)
        return -1;
    out[0] = '\0';
    if (!secret || secret_len == 0)
        return -1;

    uint8_t okm[WIFI_AP_PASSWORD_LEN];
    const char* salt = WIFI_AP_PASSWORD_HKDF_SALT;
    const char* info = WIFI_AP_PASSWORD_HKDF_INFO;

    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), secret, secret_len,
                           (const uint8_t*)info, strlen(info), okm, sizeof(okm)) != 0) {
        return -1;
    }

    for (size_t i = 0; i < WIFI_AP_PASSWORD_LEN; i++) {
        out[i] = k_alphabet[okm[i] & 0x1Fu];
    }
    out[WIFI_AP_PASSWORD_LEN] = '\0';

    memset(okm, 0, sizeof(okm));
    return 0;
}
