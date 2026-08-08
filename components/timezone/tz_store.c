#include "tz_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_keys.h"

static const char* TAG = "tz_store";

static bool read_stored_spec(char* out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_TZ, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

void tz_store_get(char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }

    char stored[BRAMBLE_TZ_SPEC_MAX];
    if (read_stored_spec(stored, sizeof(stored))) {
        if (bramble_tz_spec_valid(stored)) {
            strlcpy(out, stored, out_len);
            return;
        }
        /* A stored spec that no longer parses is ignored rather than trusted,
         * so a corrupt or truncated value degrades to real UTC instead of a
         * silently wrong local time. */
        ESP_LOGW(TAG, "Stored timezone does not parse; using %s", BRAMBLE_TZ_DEFAULT_SPEC);
    }
    strlcpy(out, BRAMBLE_TZ_DEFAULT_SPEC, out_len);
}

int tz_store_set(const char* spec) {
    if (!bramble_tz_spec_valid(spec)) {
        return -1;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &h) != ESP_OK) {
        return -2;
    }
    esp_err_t err = nvs_set_str(h, NVS_KEY_TZ, spec);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return -2;
    }

    ESP_LOGI(TAG, "Timezone set to %s", spec);
    return 0;
}

bool tz_store_is_configured(void) {
    char stored[BRAMBLE_TZ_SPEC_MAX];
    return read_stored_spec(stored, sizeof(stored)) && bramble_tz_spec_valid(stored);
}
