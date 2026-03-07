#include "include/identity.h"
#include <string.h>

/* Platform-independent collision check */
bool identity_check_collision(const bramble_identity_t* my_id, uint32_t beacon_src_addr,
                              uint32_t beacon_pubkey_hash) {
    if (my_id->address != beacon_src_addr)
        return false;
    return my_id->pubkey_hash != beacon_pubkey_hash;
}

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_log.h"

static const char* NVS_NAMESPACE = "bramble_id";

int identity_save(const bramble_identity_t* id) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return -1;
    nvs_set_blob(h, "priv", id->private_key, 32);
    nvs_set_blob(h, "pub", id->public_key, 32);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

int identity_load(bramble_identity_t* id) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return -1;
    size_t len = 32;
    if (nvs_get_blob(h, "priv", id->private_key, &len) != ESP_OK) {
        nvs_close(h);
        return -1;
    }
    len = 32;
    if (nvs_get_blob(h, "pub", id->public_key, &len) != ESP_OK) {
        nvs_close(h);
        return -1;
    }
    nvs_close(h);
    id->address = crypto_derive_address(id->public_key);
    id->pubkey_hash = crypto_derive_pubkey_hash(id->public_key);
    return 0;
}

int identity_ensure_ws_auth_token(char* token_out, size_t token_out_len) {
    if (!token_out || token_out_len < 33) {
        return -1;
    }

    nvs_handle_t h;
    if (nvs_open("bramble", NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }

    /* Default: open access (no token). Auth is opt-in — the user must
     * explicitly set a token via bramble.setAuthToken, the web flasher,
     * or the CLI `bramble auth enable` command. */
    size_t len = token_out_len;
    esp_err_t err = nvs_get_str(h, "auth_token", token_out, &len);
    if (err == ESP_OK && token_out[0] != '\0') {
        /* User has set a token — auth is active */
        nvs_close(h);
        return 0;
    }

    /* No token configured — open access */
    token_out[0] = '\0';
    nvs_close(h);
    return 1;
}

#else /* Host stubs */

int identity_save(const bramble_identity_t* id) {
    (void)id;
    return 0; /* no-op on host */
}

int identity_load(bramble_identity_t* id) {
    (void)id;
    return -1; /* no stored identity on host */
}

int identity_ensure_ws_auth_token(char* token_out, size_t token_out_len) {
    (void)token_out;
    (void)token_out_len;
    return -1;
}

#endif

int identity_generate_and_save(bramble_identity_t* id) {
    if (crypto_generate_identity(id) != 0)
        return -1;
    return identity_save(id);
}
