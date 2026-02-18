#include "ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"

static const char *TAG = "ota";

int ota_ble_start(void)
{
    ESP_LOGW(TAG, "BLE OTA not implemented yet");
    return -1;
}

int ota_wifi_start(const char *url)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "No URL provided");
        return -1;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "OTA succeeded — rebooting in 2s");
    return 0;
}

const char *ota_get_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->label : "unknown";
}

const char *ota_get_app_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}
