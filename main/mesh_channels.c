/**
 * mesh_channels.c: Channel table management (add/remove/query/default + PSK-flag NVS).
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

static const char* TAG = "mesh";

/* Forward declarations for intra-module static helpers. */
static void mesh_persist_channel_psk_flags(void);

static void mesh_persist_channel_psk_flags(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CHANNEL, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < MAX_CHANNELS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "psk%d", i);
        if (i < s_num_channels) {
            nvs_set_u8(h, key, s_channel_has_psk[i] ? 1 : 0);
        } else {
            nvs_erase_key(h, key);
        }
    }

    nvs_commit(h);
    nvs_close(h);
}

void mesh_load_channel_psk_flags(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CHANNEL, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < s_num_channels; i++) {
        /* Missing metadata defaults to "no PSK lock" for deterministic export semantics. */
        uint8_t has_psk = 0;
        /* Sized for a full int, so -Wformat-truncation holds on every target
         * (NVS keys allow up to 15 chars). */
        char key[16];
        snprintf(key, sizeof(key), "psk%d", i);
        if (nvs_get_u8(h, key, &has_psk) != ESP_OK) {
            has_psk = 0;
        }
        s_channel_has_psk[i] = (has_psk != 0);
    }

    nvs_close(h);
}

int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_num_channels >= MAX_CHANNELS) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    bramble_channel_t* ch = &s_channels[s_num_channels];
    if (psk && psk_len > 0) {
        /* Use provided PSK: treat as passphrase string */
        char psk_str[65];
        size_t copy_len = psk_len < sizeof(psk_str) - 1 ? psk_len : sizeof(psk_str) - 1;
        memcpy(psk_str, psk, copy_len);
        psk_str[copy_len] = '\0';
        channel_derive_key(psk_str, ch);
        s_channel_has_psk[s_num_channels] = true;
    } else {
        /* Generate random key */
        if (crypto_random(ch->key, BRAMBLE_KEY_SIZE) != 0) {
            /* Entropy not ready (pre-RF window): do NOT install a zeroed key.
             * Skip creating this channel; it is retried once an RF entropy
             * source is up and the gate re-opens (SEC-L1). Release the mutex
             * taken above before returning: this function's other early-return
             * (MAX_CHANNELS, above) does the same. */
            ESP_LOGW(TAG, "channel key gen deferred: entropy not ready");
            xSemaphoreGive(s_state_mutex);
            return -1;
        }
        ch->epoch = 0;
        s_channel_has_psk[s_num_channels] = false;
    }
    ch->channel_id = (uint8_t)s_num_channels;

    int idx = s_num_channels;
    s_num_channels++;

    if (name && name[0]) {
        strncpy(s_channel_names[idx], name, sizeof(s_channel_names[idx]) - 1);
        s_channel_names[idx][sizeof(s_channel_names[idx]) - 1] = '\0';
    } else {
        s_channel_names[idx][0] = '\0';
    }

    /* Persist all channels using channel_storage (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) !=
        0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS");
    }
    mesh_persist_channel_psk_flags();

    /* Catch-up budget buckets are indexed by channel position */
    channel_msg_catchup_reset();

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Channel added: idx=%d name=%s", idx, name ? name : "(unnamed)");
    return idx;
}

int mesh_remove_channel(int index) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index <= 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1; /* can't remove public channel (0) or invalid index */
    }

    /* Compact array */
    for (int i = index; i < s_num_channels - 1; i++) {
        s_channels[i] = s_channels[i + 1];
        s_channels[i].channel_id = (uint8_t)i;
        strncpy(s_channel_names[i], s_channel_names[i + 1], sizeof(s_channel_names[i]) - 1);
        s_channel_names[i][sizeof(s_channel_names[i]) - 1] = '\0';
        s_channel_has_psk[i] = s_channel_has_psk[i + 1];
    }
    s_channel_names[s_num_channels - 1][0] = '\0';
    s_channel_has_psk[s_num_channels - 1] = false;
    s_num_channels--;

    if (s_default_channel_idx == index) {
        s_default_channel_idx = 0;
    } else if (s_default_channel_idx > index) {
        s_default_channel_idx--;
    }
    if (s_default_channel_idx >= s_num_channels) {
        s_default_channel_idx = 0;
    }

    /* Persist channels after removal (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) !=
        0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS after removal");
    }
    mesh_persist_channel_psk_flags();

    /* Indices shifted: stale per-index budgets would misattribute */
    channel_msg_catchup_reset();

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Channel removed: idx=%d, %d remaining", index, s_num_channels);
    return 0;
}

int mesh_get_channel_count(void) { return s_num_channels; }

const char* mesh_get_channel_name(int index) {
    if (index < 0 || index >= s_num_channels)
        return NULL;
    if (s_channel_names[index][0])
        return s_channel_names[index];

    static char name_buf[20];
    if (index == 0)
        return "Broadcast";

    nvs_handle_t ch_nvs;
    if (nvs_open(NVS_NS_CHANNEL, NVS_READONLY, &ch_nvs) != ESP_OK) {
        return NULL;
    }

    char key_name[20];
    size_t len = sizeof(name_buf);

    /* Current storage key */
    snprintf(key_name, sizeof(key_name), "nm%d", index);
    esp_err_t err = nvs_get_str(ch_nvs, key_name, name_buf, &len);

    /* Backward-compatible legacy key */
    if (err != ESP_OK || name_buf[0] == '\0') {
        len = sizeof(name_buf);
        snprintf(key_name, sizeof(key_name), "ch%d_name", index);
        err = nvs_get_str(ch_nvs, key_name, name_buf, &len);
    }

    nvs_close(ch_nvs);
    if (err != ESP_OK || name_buf[0] == '\0') {
        return NULL;
    }
    strncpy(s_channel_names[index], name_buf, sizeof(s_channel_names[index]) - 1);
    s_channel_names[index][sizeof(s_channel_names[index]) - 1] = '\0';
    return s_channel_names[index];
}

int mesh_get_channel_security(int index, bool* has_psk, uint16_t* epoch) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    if (has_psk)
        *has_psk = s_channel_has_psk[index];
    if (epoch)
        *epoch = s_channels[index].epoch;

    xSemaphoreGive(s_state_mutex);
    return 0;
}

int mesh_set_default_channel(int index) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    s_default_channel_idx = index;

    /* Persist default channel (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) !=
        0) {
        ESP_LOGW(TAG, "Failed to persist default channel to NVS");
    }

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Default channel set to idx=%d (broadcast remains public channel 0)", index);
    return 0;
}

int mesh_get_channel_info(int* default_idx) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int count = s_num_channels;
    if (default_idx) {
        *default_idx = s_default_channel_idx;
    }
    xSemaphoreGive(s_state_mutex);
    return count;
}
