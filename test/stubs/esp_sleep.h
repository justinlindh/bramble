#ifndef ESP_SLEEP_H_STUB
#define ESP_SLEEP_H_STUB
#include <stdint.h>
static inline int esp_sleep_enable_timer_wakeup(uint64_t us) {
    (void)us;
    return 0;
}
static inline int esp_sleep_enable_ext0_wakeup(int pin, int level) {
    (void)pin;
    (void)level;
    return 0;
}
static inline void esp_deep_sleep_start(void) {}
#endif
