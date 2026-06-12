#include "ota_origin.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "ota_url.h"

static const char* TAG = "ota_origin";

#ifdef CONFIG_BRAMBLE_OTA_ALLOW_HTTP
#define OTA_ORIGIN_ALLOW_HTTP true
#else
#define OTA_ORIGIN_ALLOW_HTTP false
#endif

static bool read_stored_origin(char* out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_OTA_ORIGIN, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

void ota_origin_get(char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    char stored[OTA_URL_MAX];
    if (read_stored_origin(stored, sizeof(stored))) {
        if (ota_url_origin_valid(stored, OTA_ORIGIN_ALLOW_HTTP)) {
            strlcpy(out, stored, out_len);
            return;
        }
        /* A stored origin that no longer passes policy (e.g. http:// override
         * left behind by a dev build, now running a release build) is ignored
         * rather than trusted. */
        ESP_LOGW(TAG, "Stored OTA origin fails policy; using default");
    }
    strlcpy(out, OTA_DEFAULT_ORIGIN, out_len);
}

int ota_origin_set(const char* origin) {
    if (!ota_url_origin_valid(origin, OTA_ORIGIN_ALLOW_HTTP)) {
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) {
        return -2;
    }
    esp_err_t err = nvs_set_str(h, NVS_KEY_OTA_ORIGIN, origin);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return -2;
    }
    ESP_LOGI(TAG, "OTA origin set to %s", origin);
    return 0;
}

int ota_origin_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) {
        return -2;
    }
    esp_err_t err = nvs_erase_key(h, NVS_KEY_OTA_ORIGIN);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return -2;
    }
    ESP_LOGI(TAG, "OTA origin reset to default");
    return 0;
}

bool ota_origin_is_overridden(void) {
    char stored[OTA_URL_MAX];
    return read_stored_origin(stored, sizeof(stored));
}

int ota_resolve_artifact(const char* rel_path, char* out, size_t out_len) {
    char origin[OTA_URL_MAX];
    ota_origin_get(origin, sizeof(origin));
    return ota_url_resolve(origin, rel_path, OTA_ORIGIN_ALLOW_HTTP, out, out_len);
}
