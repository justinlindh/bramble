#ifndef ESP_HTTPS_OTA_H_STUB
#define ESP_HTTPS_OTA_H_STUB

#include "esp_http_client.h"

typedef struct {
    esp_http_client_config_t* http_config;
} esp_https_ota_config_t;

esp_err_t esp_https_ota(const esp_https_ota_config_t* ota_config);

#endif
