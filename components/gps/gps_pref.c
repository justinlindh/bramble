#include "gps_pref.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "esp_log.h"

static const char* TAG = "gps_pref";

/* Persisted GPS power preference. Default ON so a fresh GPS board behaves as
 * before; the Settings toggle (or bramble.setGpsEnabled) flips it and
 * gps_set_enabled() applies it live. */
bool gps_pref_get(void) {
    uint8_t en = 1; /* default: ON */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_GPS_EN, &en);
        nvs_close(nvs);
    }
    return en != 0;
}

void gps_pref_set(bool enabled) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_GPS_EN, enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "GPS enable saved: %d", enabled ? 1 : 0);
    }
}
