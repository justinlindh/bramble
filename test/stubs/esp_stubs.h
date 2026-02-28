#ifndef ESP_STUBS_H
#define ESP_STUBS_H

#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL           -1
#define ESP_ERR_INVALID_SIZE 0x106
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) return -1; } while(0)

#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)

#endif /* ESP_STUBS_H */
