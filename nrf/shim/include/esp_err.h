// esp_err.h shim for the nRF52840 target. Values mirror test/stubs/esp_stubs.h
// so host tests and this target agree; components only compare symbolically.
#pragma once

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x106
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107

#define ESP_ERROR_CHECK(x)                                                                         \
    do {                                                                                           \
        esp_err_t esp_err_check_rc = (x);                                                          \
        (void)esp_err_check_rc;                                                                    \
    } while (0)

static inline const char* esp_err_to_name(esp_err_t err) {
    return err == ESP_OK ? "ESP_OK" : "ESP_ERR";
}
