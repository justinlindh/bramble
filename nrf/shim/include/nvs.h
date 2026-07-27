// nvs.h shim for the nRF52840 target: the IDF NVS call surface Bramble
// actually uses, backed by a RAM entry table + byte pool (nvs_ram.c).
// RAM-backed is the P0 design; flash persistence arrives with LittleFS in P2.
// Error code values follow IDF so symbolic comparisons behave identically.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

typedef enum {
    NVS_TYPE_U8 = 0x01,
    NVS_TYPE_I8 = 0x11,
    NVS_TYPE_U16 = 0x02,
    NVS_TYPE_I16 = 0x12,
    NVS_TYPE_U32 = 0x04,
    NVS_TYPE_I32 = 0x14,
    NVS_TYPE_U64 = 0x08,
    NVS_TYPE_I64 = 0x18,
    NVS_TYPE_STR = 0x21,
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY = 0xff,
} nvs_type_t;

#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0a)

// Capacity model: entry table plus one shared byte pool. The largest real
// blob today is the serialized identity pin store, IDENTITY_STORE_BLOB_MAX =
// 2466 bytes; the pool must hold it plus the replay windows and strings.
#define NVS_RAM_MAX_ENTRIES 64
#define NVS_RAM_MAX_NS 16
#define NVS_RAM_NAME_MAX 16 // 15 chars + nul, the IDF limit
#define NVS_RAM_POOL_SIZE 8192

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value);
esp_err_t nvs_set_i8(nvs_handle_t handle, const char* key, int8_t value);
esp_err_t nvs_get_i8(nvs_handle_t handle, const char* key, int8_t* out_value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char* key, uint16_t value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char* key, uint16_t* out_value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out_value);
esp_err_t nvs_set_i32(nvs_handle_t handle, const char* key, int32_t value);
esp_err_t nvs_get_i32(nvs_handle_t handle, const char* key, int32_t* out_value);

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* inout_len);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* inout_len);

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);

// Iterator API (subset): partition name is accepted and ignored, there is a
// single RAM partition.
typedef struct nvs_opaque_iterator_t* nvs_iterator_t;
typedef struct {
    char namespace_name[NVS_RAM_NAME_MAX];
    char key[NVS_RAM_NAME_MAX];
    nvs_type_t type;
} nvs_entry_info_t;

esp_err_t nvs_entry_find(const char* part, const char* ns, nvs_type_t type, nvs_iterator_t* it);
esp_err_t nvs_entry_next(nvs_iterator_t* it);
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* out_info);
void nvs_release_iterator(nvs_iterator_t it);

#ifdef __cplusplus
}
#endif
