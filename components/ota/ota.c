#include "ota.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "ota_rollback.h"

static const char* TAG = "ota";

static char s_last_error[160];

static bool has_prefix(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void set_last_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", s_last_error);
}

const char* ota_get_last_error(void) { return s_last_error[0] ? s_last_error : NULL; }

int ota_ble_start(void) {
    ESP_LOGW(TAG, "BLE OTA not implemented yet");
    return -1;
}

static int ota_https_start(const char* url, bool allow_downgrade) {
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        set_last_error("HTTPS OTA failed to start: %s", esp_err_to_name(err));
        return -1;
    }

    /* Anti-rollback gate before committing airtime to the full download. */
    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK) {
        set_last_error("HTTPS OTA failed: could not read image description: %s",
                       esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return -1;
    }
    if (ota_rollback_gate(new_desc.version, allow_downgrade) != 0) {
        set_last_error("OTA rejected: version %s is below the anti-rollback floor",
                       new_desc.version);
        esp_https_ota_abort(handle);
        return -1;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    }
    if (err != ESP_OK) {
        set_last_error("HTTPS OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return -1;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        set_last_error("HTTPS OTA failed: incomplete download");
        esp_https_ota_abort(handle);
        return -1;
    }

    /* esp_https_ota_finish validates the image, including the appended
     * RSA-3072 signature block (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT)
     * against the public key embedded in the RUNNING app's signature block.
     * Unsigned or wrongly-signed images fail closed here. */
    err = esp_https_ota_finish(handle);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        set_last_error("OTA rejected: image signature verification failed "
                       "(unsigned or not signed by a trusted key)");
        return -1;
    }
    if (err != ESP_OK) {
        set_last_error("HTTPS OTA failed to finalize: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int ota_http_start(const char* url, bool allow_downgrade) {
#ifndef CONFIG_BRAMBLE_OTA_ALLOW_HTTP
    (void)url;
    (void)allow_downgrade;
    set_last_error("HTTP OTA disabled in release builds");
    return -1;
#else
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        set_last_error("HTTP OTA failed: client init");
        return -1;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_last_error("HTTP OTA failed: open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        set_last_error("HTTP OTA failed: status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP OTA response: status=%d content_length=%d", status, content_len);

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        set_last_error("HTTP OTA failed: no update partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        set_last_error("HTTP OTA failed: ota begin: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    char buf[4096];
    int total = 0;
    while (true) {
        int read_len = esp_http_client_read(client, buf, sizeof(buf));
        if (read_len > 0) {
            err = esp_ota_write(ota_handle, (const void*)buf, read_len);
            if (err != ESP_OK) {
                set_last_error("HTTP OTA failed: ota write: %s", esp_err_to_name(err));
                esp_ota_abort(ota_handle);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return -1;
            }
            total += read_len;
            if (content_len > 0 && total >= content_len) {
                break;
            }
            continue;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            continue;
        }

        set_last_error("HTTP OTA failed: read error len=%d", read_len);
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP OTA downloaded bytes=%d", total);
    if (total <= 0) {
        set_last_error("HTTP OTA failed: no payload downloaded");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Anti-rollback gate: the app descriptor is readable from the update
     * partition once the image body has been written. */
    esp_app_desc_t new_desc;
    err = esp_ota_get_partition_description(update_partition, &new_desc);
    if (err != ESP_OK) {
        set_last_error("HTTP OTA failed: could not read image description: %s",
                       esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        return -1;
    }
    if (ota_rollback_gate(new_desc.version, allow_downgrade) != 0) {
        set_last_error("OTA rejected: version %s is below the anti-rollback floor",
                       new_desc.version);
        esp_ota_abort(ota_handle);
        return -1;
    }

    /* esp_ota_end validates the image, including the appended signature block
     * (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT). Even on the HTTP dev
     * path there is no signature bypass. */
    err = esp_ota_end(ota_handle);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        set_last_error("OTA rejected: image signature verification failed "
                       "(unsigned or not signed by a trusted key)");
        return -1;
    }
    if (err != ESP_OK) {
        set_last_error("HTTP OTA failed: ota end: %s", esp_err_to_name(err));
        return -1;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        set_last_error("HTTP OTA failed: set boot partition: %s", esp_err_to_name(err));
        return -1;
    }

    return 0;
#endif /* CONFIG_BRAMBLE_OTA_ALLOW_HTTP */
}

int ota_wifi_start(const char* url, bool allow_downgrade) {
    if (!url || strlen(url) == 0) {
        set_last_error("No URL provided");
        return -1;
    }

    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    int rc = -1;
    if (has_prefix(url, "https://")) {
        rc = ota_https_start(url, allow_downgrade);
    } else if (has_prefix(url, "http://")) {
        rc = ota_http_start(url, allow_downgrade);
    } else {
        set_last_error("Unsupported OTA URL scheme (expected http:// or https://)");
        return -1;
    }

    if (rc != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "OTA succeeded; rebooting in 2s");
    return 0;
}

const char* ota_get_running_partition(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running ? running->label : "unknown";
}

const char* ota_get_app_version(void) {
    const esp_app_desc_t* desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}
