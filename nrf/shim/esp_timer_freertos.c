// esp_timer_get_time on the FreeRTOS tick (1ms resolution at the P0 tick
// rate; radio work that needs finer timing gets a hardware timer in P1, not
// this shim). The emulator's esp_timer_posix.c is the pattern for the full
// timer-object API when mesh_task brings its first caller in P2.
#include <FreeRTOS.h>
#include <task.h>

#include "esp_timer.h"

int64_t esp_timer_get_time(void) {
    return (int64_t)xTaskGetTickCount() * (1000000 / configTICK_RATE_HZ);
}
