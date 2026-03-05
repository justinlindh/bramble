#ifndef ESP_TIMER_H_STUB
#define ESP_TIMER_H_STUB
#include <stdint.h>
#ifndef ESP_TIMER_CUSTOM_IMPL
static inline int64_t esp_timer_get_time(void) { return 0; }
#else
int64_t esp_timer_get_time(void);
#endif
#endif
