// Mynewt HAL GPIO shim: the nrf5x PHY references these only on the optional
// debug-pin and front-end-module paths, which this build leaves disabled.
#pragma once
#include <hal/nrf_gpio.h>

static inline void hal_gpio_init_out(int pin, int val) {
    nrf_gpio_cfg_output((uint32_t)pin);
    if (val) {
        nrf_gpio_pin_set((uint32_t)pin);
    } else {
        nrf_gpio_pin_clear((uint32_t)pin);
    }
}
static inline void hal_gpio_write(int pin, int val) {
    if (val) {
        nrf_gpio_pin_set((uint32_t)pin);
    } else {
        nrf_gpio_pin_clear((uint32_t)pin);
    }
}
static inline int hal_gpio_read(int pin) { return (int)nrf_gpio_pin_read((uint32_t)pin); }
