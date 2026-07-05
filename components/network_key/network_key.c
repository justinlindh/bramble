#include "include/network_key.h"
#include "crypto.h"
#include <string.h>
#include <assert.h>

static uint8_t s_key[32];
static int s_provisioned = 0;

void network_key_set_provisioned(const uint8_t key[32]) {
    memcpy(s_key, key, 32);
    s_provisioned = 1;
}

void network_key_clear(void) {
    memset(s_key, 0, sizeof(s_key));
    s_provisioned = 0;
}

int network_key_is_provisioned(void) { return s_provisioned; }

int network_key_get(uint8_t key_out[32]) {
    if (!s_provisioned)
        return -1; /* fail-closed: no fallback, write nothing to key_out */
    memcpy(key_out, s_key, 32);
    return 0;
}

/* Longest label in use today ("bramble-receipt-v2") is 18 bytes; 32 is
 * generous headroom for future per-type labels of the same shape. */
#define NETWORK_KEY_MAC_MAX_LABEL_LEN 32

int network_key_mac(const char* label, const uint8_t* data, size_t len, uint8_t out[8]) {
    uint8_t key[32];
    if (network_key_get(key) != 0) {
        /* Fail-closed: unprovisioned. Emit the all-zero sentinel instead of an
         * HMAC over a fallback/zeroed key, and signal the caller to refuse. */
        memset(out, 0, 8);
        return -1;
    }
    assert(len <= 255);
    size_t label_len = strlen(label);
    assert(label_len <= NETWORK_KEY_MAC_MAX_LABEL_LEN);
    uint8_t buf[255 + NETWORK_KEY_MAC_MAX_LABEL_LEN];
    memcpy(buf, label, label_len);
    memcpy(buf + label_len, data, len);

    uint8_t full_mac[32];
    crypto_hmac_sha256(key, sizeof(key), buf, label_len + len, full_mac);
    memcpy(out, full_mac, 8);
    return 0;
}

void network_key_fingerprint(uint8_t out[4]) {
    uint8_t key[32];
    if (network_key_get(key) != 0) {
        /* Unprovisioned sentinel: all-zero explicitly means "no key". */
        memset(out, 0, 4);
        return;
    }
    uint8_t hash[32];
    crypto_sha256(key, sizeof(key), hash);
    memcpy(out, hash, 4);
}
