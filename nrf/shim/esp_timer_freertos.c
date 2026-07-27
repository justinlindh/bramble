// esp_timer on FreeRTOS software timers for the nRF52840 target. Port of the
// emulator's esp_timer_posix.c with the monotonic clock taken from the
// FreeRTOS tick (1ms resolution at the P0 tick rate; radio work that needs
// finer timing gets a hardware timer in P1, not this shim).
#include <stdbool.h>
#include <stdlib.h>

#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>

#include "esp_timer.h"

struct esp_timer {
    TimerHandle_t handle;
    void (*callback)(void* arg);
    void* arg;
};

int64_t esp_timer_get_time(void) {
    return (int64_t)xTaskGetTickCount() * (1000000 / configTICK_RATE_HZ);
}

static void timer_trampoline(TimerHandle_t handle) {
    struct esp_timer* timer = (struct esp_timer*)pvTimerGetTimerID(handle);
    timer->callback(timer->arg);
}

esp_err_t esp_timer_create(const esp_timer_create_args_t* create_args,
                           esp_timer_handle_t* out_handle) {
    if (create_args == NULL || out_handle == NULL || create_args->callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct esp_timer* timer = pvPortMalloc(sizeof(*timer));
    if (timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    timer->callback = create_args->callback;
    timer->arg = create_args->arg;
    // Period is a placeholder; the real period is set at start time.
    timer->handle = xTimerCreate(create_args->name ? create_args->name : "esp_timer", 1, pdFALSE,
                                 timer, timer_trampoline);
    if (timer->handle == NULL) {
        vPortFree(timer);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = timer;
    return ESP_OK;
}

static TickType_t us_to_ticks(uint64_t timeout_us) {
    uint64_t ticks = (timeout_us * configTICK_RATE_HZ + 999999ULL) / 1000000ULL;
    return (ticks == 0) ? 1 : (TickType_t)ticks;
}

static esp_err_t timer_start(esp_timer_handle_t timer, uint64_t timeout_us, BaseType_t reload) {
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTimerIsTimerActive(timer->handle) != pdFALSE) {
        return ESP_ERR_INVALID_STATE;
    }
    vTimerSetReloadMode(timer->handle, reload);
    // xTimerChangePeriod both sets the period and starts the timer.
    if (xTimerChangePeriod(timer->handle, us_to_ticks(timeout_us), portMAX_DELAY) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    return timer_start(timer, timeout_us, pdFALSE);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    return timer_start(timer, period_us, pdTRUE);
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTimerIsTimerActive(timer->handle) == pdFALSE) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xTimerStop(timer->handle, portMAX_DELAY) == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTimerDelete(timer->handle, portMAX_DELAY) != pdPASS) {
        return ESP_FAIL;
    }
    vPortFree(timer);
    return ESP_OK;
}
