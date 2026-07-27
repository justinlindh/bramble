// Bramble nRF52840 target entry point. P0 bring-up scaffold: blink the user
// LED so the board proves it is executing our code.
#include <hal/nrf_gpio.h>
#include <soc/nrfx_coredep.h>

#include "wio_wm1110_devkit.h"

int main(void)
{
    nrf_gpio_cfg_output(BOARD_PIN_LED1);
    for (;;) {
        nrf_gpio_pin_toggle(BOARD_PIN_LED1);
        nrfx_coredep_delay_us(250000);
    }
}
