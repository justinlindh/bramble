#include "ota.h"

#include <stdbool.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"

static const char* TAG = "ota";

static bool has_prefix(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int ota_ble_start(void) {
    ESP_LOGW(TAG, "BLE OTA not implemented yet");
    return -1;
}

static int ota_https_start(const char* url) {
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS OTA failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int ota_http_start(const char* url) {
#ifndef CONFIG_BRAMBLE_OTA_ALLOW_HTTP
    (void)url;
    ESP_LOGE(TAG, "HTTP OTA disabled in release builds");
    return -1;
#else
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP OTA failed: client init");
        return -1;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP OTA failed: open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP OTA failed: status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP OTA response: status=%d content_length=%d", status, content_len);

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "HTTP OTA failed: no update partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP OTA failed: ota begin: %s", esp_err_to_name(err));
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
                ESP_LOGE(TAG, "HTTP OTA failed: ota write: %s", esp_err_to_name(err));
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

        ESP_LOGE(TAG, "HTTP OTA failed: read error len=%d", read_len);
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP OTA downloaded bytes=%d", total);
    if (total <= 0) {
        ESP_LOGE(TAG, "HTTP OTA failed: no payload downloaded");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP OTA failed: ota end: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP OTA failed: set boot partition: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return 0;
#endif /* CONFIG_BRAMBLE_OTA_ALLOW_HTTP */
}

int ota_wifi_start(const char* url) {
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "No URL provided");
        return -1;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    int rc = -1;
    if (has_prefix(url, "https://")) {
        rc = ota_https_start(url);
    } else if (has_prefix(url, "http://")) {
        rc = ota_http_start(url);
    } else {
        ESP_LOGE(TAG, "Unsupported OTA URL scheme (expected http:// or https://)");
        return -1;
    }

    if (rc != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "OTA succeeded — rebooting in 2s");
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
