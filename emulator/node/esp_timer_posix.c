/*
 * Spike-only host backends for two IDF APIs that are header-only on the
 * POSIX/Linux simulator (IDF 5.4.1 provides no linux implementation):
 *
 * - esp_timer: implemented on FreeRTOS software timers (the POSIX simulation
 *   provides the timer service task). esp_timer_get_time() is a monotonic
 *   microsecond clock zeroed at process start, matching device semantics
 *   (microseconds since boot). Callbacks run in the timer service task,
 *   which matches ESP_TIMER_TASK dispatch.
 * - esp_task_wdt: no-ops. There is no hardware watchdog to feed on the
 *   host; spike_check.sh watches for runaway CPU instead.
 *
 * Only the API surface the firmware actually uses is implemented: get_time,
 * create, start_once, start_periodic, stop, delete, and wdt add/reset/delete.
 */
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

/* ── esp_timer ──────────────────────────────────────────────────────── */

struct esp_timer {
    TimerHandle_t handle;
    void (*callback)(void* arg);
    void* arg;
};

static int64_t s_boot_us;

static int64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

__attribute__((constructor)) static void esp_timer_posix_init(void) {
    s_boot_us = monotonic_us();
}

int64_t esp_timer_get_time(void) { return monotonic_us() - s_boot_us; }

static void timer_trampoline(TimerHandle_t handle) {
    struct esp_timer* timer = (struct esp_timer*)pvTimerGetTimerID(handle);
    timer->callback(timer->arg);
}

esp_err_t esp_timer_create(const esp_timer_create_args_t* create_args,
                           esp_timer_handle_t* out_handle) {
    if (create_args == NULL || out_handle == NULL || create_args->callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct esp_timer* timer = calloc(1, sizeof(*timer));
    if (timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    timer->callback = create_args->callback;
    timer->arg = create_args->arg;
    /* Period is a placeholder; the real period is set at start time. */
    timer->handle = xTimerCreate(create_args->name ? create_args->name : "esp_timer", 1, pdFALSE,
                                 timer, timer_trampoline);
    if (timer->handle == NULL) {
        free(timer);
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
    /* xTimerChangePeriod both sets the period and starts the timer. */
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
    free(timer);
    return ESP_OK;
}

/* ── esp_task_wdt ───────────────────────────────────────────────────── */

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) {
    (void)task_handle;
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }

esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle) {
    (void)task_handle;
    return ESP_OK;
}
