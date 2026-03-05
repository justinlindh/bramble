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

#else /* Host stubs */

int identity_save(const bramble_identity_t* id) {
    (void)id;
    return 0; /* no-op on host */
}

int identity_load(bramble_identity_t* id) {
    (void)id;
    return -1; /* no stored identity on host */
}

#endif

int identity_generate_and_save(bramble_identity_t* id) {
    if (crypto_generate_identity(id) != 0)
        return -1;
    return identity_save(id);
}
