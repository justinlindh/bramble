#ifndef ESP_OTA_OPS_H_STUB
#define ESP_OTA_OPS_H_STUB

#include "esp_stubs.h"

typedef struct {
    const char* label;
} esp_partition_t;

typedef int esp_ota_handle_t;

#define OTA_SIZE_UNKNOWN -1
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from);
esp_err_t esp_ota_begin(const esp_partition_t* partition, int image_size,
                        esp_ota_handle_t* out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, int size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
const esp_partition_t* esp_ota_get_running_partition(void);

const char* esp_err_to_name(esp_err_t err);

typedef struct {
    const char* version;
    uint32_t secure_version;
} esp_app_desc_t;

const esp_app_desc_t* esp_app_get_description(void);
esp_err_t esp_ota_get_partition_description(const esp_partition_t* partition,
                                            esp_app_desc_t* app_desc);

#endif
