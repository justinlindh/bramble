// esp_log.h shim for the nRF52840 target. Same line format as IDF
// ("I (1234) tag: message") so existing log tooling and habits carry over.
#pragma once

#include <stdarg.h>

// IDF's esp_log.h transitively provides sdkconfig.h; components rely on that
// (rpc_dispatcher.c reads CONFIG_ values with no direct include).
#include "sdkconfig.h"

void bramble_log_write(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) bramble_log_write('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) bramble_log_write('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) bramble_log_write('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) bramble_log_write('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
