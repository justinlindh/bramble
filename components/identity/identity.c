#include "include/identity.h"
#include <string.h>

/* Platform-independent collision check */
bool identity_check_collision(const bramble_identity_t* my_id, uint32_t beacon_src_addr,
                              uint32_t beacon_pubkey_hash) {
    if (my_id->address != beacon_src_addr)
        return false;
    return my_id->pubkey_hash != beacon_pubkey_hash;
}

/* Blob store keys. "priv"/"pub" (X25519) predate Phase 1; a store holding
 * only those two is an old identity and gets the Ed25519 migration in
 * identity_load(). */
#define ID_KEY_X25519_PRIV "priv"
#define ID_KEY_X25519_PUB "pub"
#define ID_KEY_ED25519_PUB "ed_pub"
#define ID_KEY_ED25519_PRIV "ed_priv"

/* Per-platform blob store: device = NVS, host = in-memory (unit tests).
 * id_store_read() is exact-length and fail-closed: a missing key or a
 * length mismatch is an error. */
static int id_store_read(const char* key, uint8_t* buf, size_t len);
static int id_store_write(const char* key, const uint8_t* buf, size_t len);

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "esp_random.h"
#include "esp_log.h"

#define NVS_NAMESPACE NVS_NS_IDENTITY

static int id_store_read(const char* key, uint8_t* buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return -1;
    size_t got = len;
    int ret = (nvs_get_blob(h, key, buf, &got) == ESP_OK && got == len) ? 0 : -1;
    nvs_close(h);
    return ret;
}

static int id_store_write(const char* key, const uint8_t* buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int ret = (nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK) ? 0 : -1;
    nvs_close(h);
    return ret;
}

int identity_ensure_ws_auth_token(char* token_out, size_t token_out_len) {
    if (!token_out || token_out_len < 33) {
        return -1;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }

    /* Explicit opt-out: only an authenticated bramble.setAuthToken call
     * with an empty token sets this flag. Default is auth required. */
    uint8_t auth_disabled = 0;
    nvs_get_u8(h, NVS_KEY_AUTH_OFF, &auth_disabled);
    if (auth_disabled) {
        token_out[0] = '\0';
        nvs_close(h);
        return 0; /* auth explicitly disabled, no token */
    }

    size_t len = token_out_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_AUTH_TOKEN, token_out, &len);
    if (err == ESP_OK && token_out[0] != '\0') {
        nvs_close(h);
        return 0;
    }

    /* First boot: generate a per-device token so WS/BLE RPC is closed by
     * default. 16 bytes from the hardware RNG, hex-encoded to 32 chars.
     *
     * ENTROPY ORDER CONTRACT: esp_fill_random() is only fully entropic
     * once an RF subsystem (Wi-Fi or BT) is running. Every call path into
     * this function runs after RF bring-up:
     *   - ws_server_start() -> ws_server_load_token(): called after
     *     esp_wifi_start() (wifi_manager AP/GOT_IP paths and app_main's
     *     post-wifi_manager_init call).
     *   - app_main BLE branch: ws_server_load_token() after
     *     ble_server_start() (NimBLE controller running).
     *   - rpc_set_auth_token(): runtime, transports already up.
     * Do not add a call site that runs before RF init. */
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < sizeof(rnd); i++) {
        token_out[i * 2] = hex[(rnd[i] >> 4) & 0x0F];
        token_out[i * 2 + 1] = hex[rnd[i] & 0x0F];
    }
    token_out[32] = '\0';

    err = nvs_set_str(h, NVS_KEY_AUTH_TOKEN, token_out);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        /* Generation failed: report an error so callers fail CLOSED
         * (no token != open access; see ws_server_load_token). */
        token_out[0] = '\0';
        return -1;
    }

    /* Log to the serial console on purpose: serial is the trusted pairing
     * bootstrap (physical access = trust), and this is how users without
     * the CLI can read the token; `bramble pair` is the scripted path. */
    ESP_LOGW("identity", "Generated device auth token (retrieve with `bramble pair`): %s",
             token_out);
    return 1;
}

#else

/* Host: in-memory blob store so unit tests exercise the shared
 * save/load/migration logic below. Starts empty, like a fresh flash. */

#define ID_HOST_BLOB_MAX 4
#define ID_HOST_BLOB_CAP 64

static struct {
    char key[16];
    uint8_t data[ID_HOST_BLOB_CAP];
    size_t len;
} s_host_blobs[ID_HOST_BLOB_MAX];
static int s_host_blob_count;

void identity_host_store_reset(void) { s_host_blob_count = 0; }

static int id_store_read(const char* key, uint8_t* buf, size_t len) {
    for (int i = 0; i < s_host_blob_count; i++) {
        if (strcmp(s_host_blobs[i].key, key) == 0) {
            if (s_host_blobs[i].len != len)
                return -1;
            memcpy(buf, s_host_blobs[i].data, len);
            return 0;
        }
    }
    return -1;
}

static int id_store_write(const char* key, const uint8_t* buf, size_t len) {
    if (len > ID_HOST_BLOB_CAP || strlen(key) >= sizeof(s_host_blobs[0].key))
        return -1;
    int slot = s_host_blob_count;
    for (int i = 0; i < s_host_blob_count; i++) {
        if (strcmp(s_host_blobs[i].key, key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot == s_host_blob_count) {
        if (s_host_blob_count == ID_HOST_BLOB_MAX)
            return -1;
        s_host_blob_count++;
        strcpy(s_host_blobs[slot].key, key);
    }
    memcpy(s_host_blobs[slot].data, buf, len);
    s_host_blobs[slot].len = len;
    return 0;
}

int identity_ensure_ws_auth_token(char* token_out, size_t token_out_len) {
    (void)token_out;
    (void)token_out_len;
    return -1;
}

#endif

/* --- Shared save/load/migration (device NVS and host store) -------------- */

int identity_save(const bramble_identity_t* id) {
    if (id_store_write(ID_KEY_X25519_PRIV, id->private_key, BRAMBLE_KEY_SIZE) != 0 ||
        id_store_write(ID_KEY_X25519_PUB, id->public_key, BRAMBLE_KEY_SIZE) != 0 ||
        id_store_write(ID_KEY_ED25519_PUB, id->ed25519_public_key, BRAMBLE_ED25519_PUBKEY_SIZE) !=
            0 ||
        id_store_write(ID_KEY_ED25519_PRIV, id->ed25519_private_key, BRAMBLE_ED25519_SECKEY_SIZE) !=
            0) {
        return -1;
    }
    return 0;
}

int identity_load(bramble_identity_t* id) {
    if (id_store_read(ID_KEY_X25519_PRIV, id->private_key, BRAMBLE_KEY_SIZE) != 0 ||
        id_store_read(ID_KEY_X25519_PUB, id->public_key, BRAMBLE_KEY_SIZE) != 0) {
        return -1;
    }

    if (id_store_read(ID_KEY_ED25519_PUB, id->ed25519_public_key, BRAMBLE_ED25519_PUBKEY_SIZE) !=
            0 ||
        id_store_read(ID_KEY_ED25519_PRIV, id->ed25519_private_key, BRAMBLE_ED25519_SECKEY_SIZE) !=
            0) {
        /* MIGRATION (Phase 1): an old store holds only the X25519 identity.
         * Keep it (the address stays X25519-derived and thus stable) and
         * generate + persist a fresh Ed25519 keypair for it. Fail closed:
         * no keygen or persist success, no identity. */
        if (crypto_ed25519_keypair(id->ed25519_public_key, id->ed25519_private_key) != 0)
            return -1;
        if (id_store_write(ID_KEY_ED25519_PUB, id->ed25519_public_key,
                           BRAMBLE_ED25519_PUBKEY_SIZE) != 0 ||
            id_store_write(ID_KEY_ED25519_PRIV, id->ed25519_private_key,
                           BRAMBLE_ED25519_SECKEY_SIZE) != 0) {
            return -1;
        }
    }

    /* Phase 4 rebind: address/pubkey_hash derive from the Ed25519 identity
     * key. For a migrated (previously X25519-only) store this is the flag
     * day: the node comes up with a NEW address derived from its freshly
     * generated Ed key. Deliberate and owner-approved (pre-alpha): peers'
     * pins are RAM-only and re-establish via attestation TOFU. */
    id->address = crypto_derive_address(id->ed25519_public_key);
    id->pubkey_hash = crypto_derive_pubkey_hash(id->ed25519_public_key);
    return 0;
}

int identity_generate_and_save(bramble_identity_t* id) {
    if (crypto_generate_identity(id) != 0)
        return -1;
    return identity_save(id);
}
