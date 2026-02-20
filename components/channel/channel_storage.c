/**
 * Channel persistence using NVS (Phase 1)
 * Stores channel keys, names, and default channel index robustly.
 */

#include "include/channel_storage.h"
#include "include/channel_key.h"
#include "include/channel_msg.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE "bramble_ch"
#define TAG "ch_storage"

int channel_storage_init(void) {
    /* NVS init is already done in main, this is a no-op */
    return 0;
}

int channel_storage_save(const bramble_channel_t *channels, int num_channels,
                         const char names[][20], int default_channel_idx) {
    if (!channels || !names || num_channels < 0 || num_channels > MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid params: channels=%p, names=%p, num=%d", 
                 channels, names, num_channels);
        return -1;
    }

    if (default_channel_idx < 0 || default_channel_idx >= MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid default_channel_idx: %d", default_channel_idx);
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Save count */
    err = nvs_set_u8(h, "count", (uint8_t)num_channels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save count: %s", esp_err_to_name(err));
        nvs_close(h);
        return -1;
    }

    /* Save default channel index */
    err = nvs_set_u8(h, "default", (uint8_t)default_channel_idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save default: %s", esp_err_to_name(err));
    }

    /* Save each channel blob */
    char key[16];  /* Increased size to avoid truncation warnings */
    for (int i = 0; i < num_channels; i++) {
        snprintf(key, sizeof(key), "ch%d", i);
        err = nvs_set_blob(h, key, &channels[i], sizeof(bramble_channel_t));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel %d: %s", i, esp_err_to_name(err));
        }
    }

    /* Save channel names */
    for (int i = 0; i < num_channels; i++) {
        if (names[i][0] != '\0') {
            snprintf(key, sizeof(key), "nm%d", i);
            err = nvs_set_str(h, key, names[i]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save name %d: %s", i, esp_err_to_name(err));
            }
        }
    }

    /* Commit */
    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %d channels to NVS (default=%d)", 
                 num_channels, default_channel_idx);
        return 0;
    } else {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return -1;
    }
}

int channel_storage_load(bramble_channel_t *channels, int *num_channels,
                         char names[][20], int *default_channel_idx) {
    if (!channels || !num_channels || !names || !default_channel_idx) {
        ESP_LOGE(TAG, "Invalid params: channels=%p, num=%p, names=%p, default=%p",
                 channels, num_channels, names, default_channel_idx);
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (no saved channels): %s", esp_err_to_name(err));
        *num_channels = 0;
        *default_channel_idx = 0;
        return -1;
    }

    /* Load count */
    uint8_t count = 0;
    err = nvs_get_u8(h, "count", &count);
    if (err != ESP_OK || count > MAX_CHANNELS) {
        ESP_LOGW(TAG, "Invalid or missing channel count");
        nvs_close(h);
        *num_channels = 0;
        *default_channel_idx = 0;
        return -1;
    }

    /* Load default channel index */
    uint8_t def_idx = 0;
    err = nvs_get_u8(h, "default", &def_idx);
    if (err == ESP_OK && def_idx < MAX_CHANNELS) {
        *default_channel_idx = def_idx;
    } else {
        *default_channel_idx = 0;
    }

    /* Load each channel blob */
    char key[16];  /* Increased size to avoid truncation warnings */
    int loaded = 0;
    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "ch%d", i);
        size_t len = sizeof(bramble_channel_t);
        err = nvs_get_blob(h, key, &channels[i], &len);
        if (err == ESP_OK && len == sizeof(bramble_channel_t)) {
            loaded++;
        } else {
            ESP_LOGW(TAG, "Failed to load channel %d: %s", i, esp_err_to_name(err));
        }
    }

    /* Load channel names */
    for (int i = 0; i < loaded; i++) {
        snprintf(key, sizeof(key), "nm%d", i);
        size_t len = 20;
        err = nvs_get_str(h, key, names[i], &len);
        if (err != ESP_OK) {
            /* No name stored, use empty string */
            names[i][0] = '\0';
        }
    }

    nvs_close(h);
    *num_channels = loaded;

    if (loaded > 0) {
        ESP_LOGI(TAG, "Loaded %d channels from NVS (default=%d)", 
                 loaded, *default_channel_idx);
        return 0;
    } else {
        ESP_LOGW(TAG, "No channels loaded");
        return -1;
    }
}

void channel_storage_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Cleared all channel storage");
    }
}

#else /* Host stubs for unit tests */

int channel_storage_init(void) { 
    return 0; 
}

int channel_storage_save(const bramble_channel_t *channels, int num_channels,
                         const char names[][20], int default_channel_idx) {
    (void)channels; 
    (void)num_channels;
    (void)names;
    (void)default_channel_idx;
    return 0;
}

int channel_storage_load(bramble_channel_t *channels, int *num_channels,
                         char names[][20], int *default_channel_idx) {
    (void)channels;
    (void)names;
    if (num_channels) *num_channels = 0;
    if (default_channel_idx) *default_channel_idx = 0;
    return -1;
}

void channel_storage_clear(void) {
}

#endif /* ESP_PLATFORM */
