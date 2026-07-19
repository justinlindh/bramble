#ifndef ESP_TASK_WDT_H_STUB
#define ESP_TASK_WDT_H_STUB

/* Host stub for the ESP-IDF task watchdog. Feeding it is a no-op off-target. */
static inline int esp_task_wdt_reset(void) { return 0; }
static inline int esp_task_wdt_add(void* handle) {
    (void)handle;
    return 0;
}
static inline int esp_task_wdt_delete(void* handle) {
    (void)handle;
    return 0;
}

#endif
