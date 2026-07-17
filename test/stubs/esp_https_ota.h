#ifndef ESP_HTTPS_OTA_H_STUB
#define ESP_HTTPS_OTA_H_STUB

#include <stdbool.h>

#include "esp_http_client.h"
#include "esp_ota_ops.h"

typedef struct {
    esp_http_client_config_t* http_config;
} esp_https_ota_config_t;

typedef void* esp_https_ota_handle_t;

#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x9001

esp_err_t esp_https_ota(const esp_https_ota_config_t* ota_config);

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t* ota_config,
                              esp_https_ota_handle_t* handle);
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t* new_app_info);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle);
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle);
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle);

#endif
