// esp_timer shim for the nRF52840 target, implemented on FreeRTOS software
// timers (nrf/shim/esp_timer_freertos.c), mirroring the emulator's
// esp_timer_posix.c semantics.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer* esp_timer_handle_t;

typedef enum {
    ESP_TIMER_TASK,
} esp_timer_dispatch_t;

typedef struct {
    void (*callback)(void* arg);
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t* create_args,
                           esp_timer_handle_t* out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

#ifdef __cplusplus
}
#endif
