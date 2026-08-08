#include "ble_pairing_store.h"

#include "nvs.h"
#include "nvs_keys.h"

int ble_pairing_store_set(uint32_t passkey) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return -1;
    }
    err = nvs_set_u32(nvs, NVS_KEY_BLE_PASSKEY, passkey);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK ? 0 : -1;
}

int ble_pairing_store_clear(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return -1;
    }
    err = nvs_erase_key(nvs, NVS_KEY_BLE_PASSKEY);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_FAIL) {
        err = ESP_OK; /* clearing an unset key is success */
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK ? 0 : -1;
}

bool ble_pairing_store_get(uint32_t* out) {
    if (out == NULL) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u32(nvs, NVS_KEY_BLE_PASSKEY, out);
    nvs_close(nvs);
    return err == ESP_OK;
}

bool ble_pairing_store_is_set(void) {
    uint32_t v;
    return ble_pairing_store_get(&v);
}
