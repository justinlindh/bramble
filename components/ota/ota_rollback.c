#include "ota_rollback.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "ota_rollback_policy.h"
#include "ota_version.h"
#include "sdkconfig.h"

#if CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
#include "esp_efuse.h"
#endif
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#include "esp_ota_ops.h"
#endif

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
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK selects APP_ROLLBACK_ENABLE: a freshly
     * OTA'd image boots in pending-verify state and reverts on the next reboot
     * unless the app confirms it is operable. Reaching this call (after NVS
     * init, from the main bring-up path) is the current definition of a
     * successful boot. Confirming is ALSO the moment IDF ratchets the eFuse
     * secure-version floor up to the running app's secure_version, so until
     * this call succeeds the bootloader can still fall back to the previous
     * app. */
    esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_err == ESP_OK) {
        ESP_LOGI(TAG, "Boot confirmed valid (rollback cancelled, secure version ratchet applied)");
    } else if (mark_err != ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE just means the image was not pending verification (a
         * normal boot of an already-confirmed image); anything else is worth a
         * warning because an unconfirmed image reverts on the next reboot. */
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s",
                 esp_err_to_name(mark_err));
    }
#endif

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

int ota_rollback_gate(const char* new_version, uint32_t candidate_secure_version,
                      bool allow_downgrade) {
#if CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    /* Hardware floor first. esp_efuse_check_secure_version returns true when the
     * image's secure_version is at or above the burned eFuse value, i.e. when
     * the bootloader would let it run. A sub-floor image is rejected outright,
     * regardless of allow_downgrade: installing it would only brick the device
     * on the next boot. This check is independent of the semver string, so it
     * also blocks an unparseable-versioned image that fails the hardware floor. */
    bool clears_secure_floor = esp_efuse_check_secure_version(candidate_secure_version);
    if (ota_rollback_secure_floor_blocks(true, clears_secure_floor)) {
        ESP_LOGE(TAG,
                 "OTA rejected: image %s (secure_version %lu) is below the eFuse "
                 "anti-rollback floor; it would fail to boot",
                 new_version ? new_version : "(null)", (unsigned long)candidate_secure_version);
        return -1;
    }
#else
    (void)candidate_secure_version;
#endif

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
