// Bramble nRF52840 target entry point. P0 bring-up: console banner plus LED
// blink proving code execution and the UART route.
#include <hal/nrf_gpio.h>
#include <soc/nrfx_coredep.h>

#include "console.h"
#include "esp_log.h"
#include "wio_wm1110_devkit.h"

#ifndef BRAMBLE_GIT_DESCRIBE
#define BRAMBLE_GIT_DESCRIBE "unknown"
#endif

static const char* TAG = "main";

int main(void) {
    console_init();
    ESP_LOGI(TAG, "Bramble nRF52840 P0 %s booted", BRAMBLE_GIT_DESCRIBE);

    nrf_gpio_cfg_output(BOARD_PIN_LED1);
    uint32_t beats = 0;
    for (;;) {
        nrf_gpio_pin_toggle(BOARD_PIN_LED1);
        nrfx_coredep_delay_us(250000);
        beats++;
        if ((beats % 8) == 0) {
            ESP_LOGI(TAG, "alive, %lu blinks", (unsigned long)beats);
        }
    }
}
