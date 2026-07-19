#include "include/network_key.h"
#include "crypto.h"
#include <string.h>

static uint8_t s_key[32];
static int s_provisioned = 0;

/*
 * Per-platform persisted key store: device = NVS (NVS_NS_NETKEY), host =
 * in-memory so network_key_load_from_nvs round-trips in unit tests. Both are
 * exact-length and fail-closed: a missing or wrong-length blob is "no key".
 */
static int netkey_store_read(uint8_t key_out[32]);
static int netkey_store_write(const uint8_t key[32]);

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_keys.h"

static int netkey_store_read(uint8_t key_out[32]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NETKEY, NVS_READONLY, &h) != ESP_OK)
        return -1;
    size_t len = 32;
    int ret = (nvs_get_blob(h, NVS_KEY_NETKEY, key_out, &len) == ESP_OK && len == 32) ? 0 : -1;
    nvs_close(h);
    return ret;
}

static int netkey_store_write(const uint8_t key[32]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NETKEY, NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int ret =
        (nvs_set_blob(h, NVS_KEY_NETKEY, key, 32) == ESP_OK && nvs_commit(h) == ESP_OK) ? 0 : -1;
    nvs_close(h);
    return ret;
}

#else

/* Host: in-memory persisted key so load_from_nvs is unit-testable. Starts
 * empty, like a fresh flash; network_key_host_store_reset() clears it between
 * tests. */
static uint8_t s_host_key[32];
static int s_host_has_key = 0;

void network_key_host_store_reset(void) {
    memset(s_host_key, 0, sizeof(s_host_key));
    s_host_has_key = 0;
}

static int netkey_store_read(uint8_t key_out[32]) {
    if (!s_host_has_key)
        return -1;
    memcpy(key_out, s_host_key, 32);
    return 0;
}

static int netkey_store_write(const uint8_t key[32]) {
    memcpy(s_host_key, key, 32);
    s_host_has_key = 1;
    return 0;
}

#endif

/* Provision in memory only (no persist); shared by the set path and the
 * load-from-store path (which must not re-write what it just read). */
static void netkey_set_inmem(const uint8_t key[32]) {
    memcpy(s_key, key, 32);
    s_provisioned = 1;
}

void network_key_set_provisioned(const uint8_t key[32]) {
    netkey_set_inmem(key);
    /* Persist so the key survives reboot. In-memory state is authoritative
     * for the running node; a store-write failure does not un-provision. */
    netkey_store_write(key);
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

int network_key_generate_provision(uint8_t key_out[32]) {
    /* Draw into a scratch buffer first so an entropy-gate failure (SEC-L1)
     * provisions nothing and leaves key_out untouched, mirroring
     * crypto_generate_identity's fail-closed pattern. */
    uint8_t key[32];
    if (crypto_random(key, sizeof(key)) != 0)
        return -1; /* entropy gate shut: provision nothing */
    network_key_set_provisioned(key);
    memcpy(key_out, key, sizeof(key));
    return 0;
}

int network_key_load_from_nvs(void) {
    uint8_t key[32];
    if (netkey_store_read(key) != 0)
        return -1; /* none stored */
    netkey_set_inmem(key);
    return 0;
}

/* Longest label in use today ("bramble-receipt-v2") is 18 bytes; 32 is
 * generous headroom for future per-type labels of the same shape. */
#define NETWORK_KEY_MAC_MAX_LABEL_LEN 32
/* Largest MAC'd body. Every caller today passes a compile-time-constant
 * length well under this, but the check below is a real runtime one: this
 * runs on the RX path, and assert() compiles out under NDEBUG. */
#define NETWORK_KEY_MAC_MAX_DATA_LEN 255

int network_key_mac(const char* label, const uint8_t* data, size_t len, uint8_t out[8]) {
    /* Bound the scratch buffer BEFORE touching key material, so an
     * out-of-range request never even loads the key. Fail-closed like the
     * unprovisioned case: all-zero sentinel out, nonzero return. */
    size_t label_len = strlen(label);
    if (len > NETWORK_KEY_MAC_MAX_DATA_LEN || label_len > NETWORK_KEY_MAC_MAX_LABEL_LEN) {
        memset(out, 0, 8);
        return -1;
    }

    uint8_t key[32];
    if (network_key_get(key) != 0) {
        /* Fail-closed: unprovisioned. Emit the all-zero sentinel instead of an
         * HMAC over a fallback/zeroed key, and signal the caller to refuse. */
        memset(out, 0, 8);
        return -1;
    }

    uint8_t buf[NETWORK_KEY_MAC_MAX_DATA_LEN + NETWORK_KEY_MAC_MAX_LABEL_LEN];
    memcpy(buf, label, label_len);
    memcpy(buf + label_len, data, len);

    uint8_t full_mac[32];
    crypto_hmac_sha256(key, sizeof(key), buf, label_len + len, full_mac);
    memcpy(out, full_mac, 8);
    /* Do not leave the network key on the RX-path stack. crypto_secure_wipe,
     * not memset: the compiler is free to elide a memset whose result is
     * never read (see crypto.h). */
    crypto_secure_wipe(key, sizeof(key));
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
    crypto_secure_wipe(key, sizeof(key));
}
