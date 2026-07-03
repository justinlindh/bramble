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
    if (s_provisioned) {
        memcpy(key_out, s_key, 32);
        return 0;
    }
    /* UNPROVISIONED FALLBACK (staging, not a fix; see network_key.h): every
     * node ships with the identical BRAMBLE_PUBLIC_CHANNEL_PSK, a public
     * compile-time constant, so this derived key is not a secret. It exists
     * so the MAC machinery has a key to run against before real
     * provisioning lands, not to authenticate anything against an
     * adversary who has read this source. */
    const char *salt = "bramble-netkey-fallback";
    const char *psk = BRAMBLE_PUBLIC_CHANNEL_PSK;
    return crypto_hkdf_sha256((const uint8_t *)salt, strlen(salt), (const uint8_t *)psk,
                              strlen(psk), NULL, 0, key_out, 32);
}

/* Longest label in use today ("bramble-receipt-v2") is 18 bytes; 32 is
 * generous headroom for future per-type labels of the same shape. */
#define NETWORK_KEY_MAC_MAX_LABEL_LEN 32

void network_key_mac(const char *label, const uint8_t *data, size_t len, uint8_t out[8]) {
    assert(len <= 255);
    size_t label_len = strlen(label);
    assert(label_len <= NETWORK_KEY_MAC_MAX_LABEL_LEN);
    uint8_t buf[255 + NETWORK_KEY_MAC_MAX_LABEL_LEN];
    memcpy(buf, label, label_len);
    memcpy(buf + label_len, data, len);

    uint8_t key[32];
    network_key_get(key);
    uint8_t full_mac[32];
    crypto_hmac_sha256(key, sizeof(key), buf, label_len + len, full_mac);
    memcpy(out, full_mac, 8);
}
