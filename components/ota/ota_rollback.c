#include "ota_rollback.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "ota_rollback_policy.h"
#include "ota_version.h"

static const char* TAG = "ota_rollback";

#define OTA_FLOOR_MAX 48

static bool read_floor(char* out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_OTA_VER_FLOOR, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

static int write_floor(const char* version) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_str(h, NVS_KEY_OTA_VER_FLOOR, version);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

void ota_rollback_note_boot(void) {
    const esp_app_desc_t* desc = esp_app_get_description();
    if (!desc) {
        return;
    }

    ota_semver_t running;
    if (!ota_version_parse(desc->version, &running)) {
        ESP_LOGW(TAG, "Running version '%s' is not semver; floor unchanged", desc->version);
        return;
    }

    char floor_str[OTA_FLOOR_MAX];
    const char* floor = read_floor(floor_str, sizeof(floor_str)) ? floor_str : NULL;

    if (!ota_rollback_should_raise_floor(desc->version, floor)) {
        return; /* unparseable running version, or floor already at or above it */
    }

    if (write_floor(desc->version) == 0) {
        ESP_LOGI(TAG, "Anti-rollback floor raised to %s", desc->version);
    } else {
        ESP_LOGW(TAG, "Failed to persist anti-rollback floor");
    }
}

int ota_rollback_gate(const char* new_version, bool allow_downgrade) {
    char floor_str[OTA_FLOOR_MAX];
    const char* floor = read_floor(floor_str, sizeof(floor_str)) ? floor_str : NULL;

    switch (ota_rollback_decide(new_version, floor, allow_downgrade)) {
    case OTA_ROLLBACK_ACCEPT:
        return 0;

    case OTA_ROLLBACK_ACCEPT_UNPARSEABLE:
        ESP_LOGW(TAG, "Image version '%s' unparseable; accepted via allow_downgrade",
                 new_version ? new_version : "(null)");
        return 0;

    case OTA_ROLLBACK_REJECT_UNPARSEABLE:
        ESP_LOGE(TAG, "OTA rejected: image version '%s' is not semver (fail closed)",
                 new_version ? new_version : "(null)");
        return -1;

    case OTA_ROLLBACK_REJECT_BELOW_FLOOR:
        ESP_LOGE(TAG,
                 "OTA rejected: image version %s is below the anti-rollback floor %s "
                 "(pass allow_downgrade to override)",
                 new_version, floor_str);
        return -1;

    case OTA_ROLLBACK_ACCEPT_LOWER_FLOOR:
        break;
    }

    /* Deliberate downgrade: lower the floor so the device is not stranded
     * under the old floor after rebooting into the older firmware.
     *
     * Known window, accepted: this runs at gate time, BEFORE signature
     * verification, against the unverified image descriptor. A failed
     * downgrade therefore leaves the floor lowered until the next boot of
     * the (unchanged) running image re-raises it via
     * ota_rollback_note_boot. Reaching this path at all requires the
     * device auth token (allow_downgrade rides an authenticated RPC), so
     * the exposure is a token holder lowering their own floor, which they
     * can do deliberately anyway. */
    if (write_floor(new_version) == 0) {
        ESP_LOGW(TAG, "Deliberate downgrade: floor lowered from %s to %s", floor_str, new_version);
    } else {
        ESP_LOGW(TAG, "Downgrade accepted but floor update failed (floor stays %s)", floor_str);
    }
    return 0;
}

bool ota_rollback_get_floor(char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    return read_floor(out, out_len);
}
