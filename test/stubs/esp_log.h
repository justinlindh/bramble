#ifndef ESP_LOG_H_STUB
#define ESP_LOG_H_STUB
#include "esp_stubs.h"

/* Log level constants used by esp_log_level_set() */
typedef enum {
    ESP_LOG_NONE    = 0,
    ESP_LOG_ERROR   = 1,
    ESP_LOG_WARN    = 2,
    ESP_LOG_INFO    = 3,
    ESP_LOG_DEBUG   = 4,
    ESP_LOG_VERBOSE = 5,
} esp_log_level_t;

static inline void esp_log_level_set(const char* tag, esp_log_level_t level) {
    (void)tag;
    (void)level;
}

#endif
