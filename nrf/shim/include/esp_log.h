// esp_log.h shim for the nRF52840 target. Same line format as IDF
// ("I (1234) tag: message") so existing log tooling and habits carry over.
#pragma once

#include <stdarg.h>

void bramble_log_write(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) bramble_log_write('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) bramble_log_write('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) bramble_log_write('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) bramble_log_write('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

// Some components use the ESP_LOG_LEVEL forms or esp_log_timestamp directly.
unsigned int esp_log_timestamp(void);
