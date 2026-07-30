#include "esp_system.h"

#include <FreeRTOS.h>
#include <timers.h>

#include <nrfx.h>

void esp_restart(void) { NVIC_SystemReset(); }

uint32_t esp_get_free_heap_size(void) { return (uint32_t)xPortGetFreeHeapSize(); }

static void enter_dfu_timer_cb(TimerHandle_t t) {
    (void)t;
    /* Adafruit bootloader DFU_MAGIC_UF2_RESET: stay resident with the UF2
     * volume after reset instead of booting the app. */
    NRF_POWER->GPREGRET = 0x57;
    __DSB();
    NVIC_SystemReset();
}

/* Strong override of the weak default in rpc_methods.c. Delayed so the RPC
 * response reaches the client before the link drops. */
int bramble_platform_enter_dfu(void) {
    TimerHandle_t t = xTimerCreate("dfu", pdMS_TO_TICKS(500), pdFALSE, NULL, enter_dfu_timer_cb);
    if (t == NULL || xTimerStart(t, pdMS_TO_TICKS(100)) != pdPASS) {
        /* No timer, no grace period: enter DFU anyway, losing the response.
         * The client treats the disconnect as success. */
        enter_dfu_timer_cb(NULL);
    }
    return 0;
}
