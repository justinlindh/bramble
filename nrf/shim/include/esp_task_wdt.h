// esp_task_wdt shim for the nRF52840 target: no-ops, the emulator precedent.
// A real watchdog (nRF WDT peripheral) is P3 power/robustness work.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline esp_err_t esp_task_wdt_add(void* task_handle) {
    (void)task_handle;
    return ESP_OK;
}

static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }

static inline esp_err_t esp_task_wdt_delete(void* task_handle) {
    (void)task_handle;
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
