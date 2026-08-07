#ifndef BOARD_T1000E_H
#define BOARD_T1000E_H

#include "board_config.h"

/*
 * Seeed SenseCAP T1000-E card tracker: the nRF52840 target's board profile
 * (bare-metal FreeRTOS, LR1110 radio, no display, consoleless; BLE is the
 * interface).
 *
 * The pin fields are deliberately all -1: the real pin mapping is owned by
 * the port (nrf/boards/t1000e.h) and the drivers that consume THESE fields
 * are the ESP-IDF ones, which do not build for nRF. This struct exists for
 * identity (name, short_name) and the capability mask. GNSS and battery
 * (including charge/VBUS detect) are both brought up on this hardware, but
 * through the nRF port's own board header and drivers (nrf/boards/t1000e.h,
 * nrf/shim/gps_t1000e.c, nrf/shim/battery_t1000e.c), never through this
 * struct. The buzzer and other sensors on the hardware are not brought up
 * yet, on either port.
 */
static const bramble_board_config_t board_t1000e = {
    .name = "Seeed SenseCAP T1000-E",
    .short_name = "t1000e",
    .capabilities = BOARD_CAP_GPS,

    .peripheral_power_pin = -1,

    .spi = {.mosi = -1, .miso = -1, .sck = -1},
    .spi_host = 0,
    .spi_max_transfer_sz = 256,

    .radio = {.cs = -1, .rst = -1, .busy = -1, .dio1 = -1},
    .radio_osc = RADIO_OSC_TCXO_DIO3,
    .radio_tcxo_voltage = 1.6f,
    .radio_reg = RADIO_REG_DCDC,

    .display_width = 0,
    .display_height = 0,
    .i2c_display = {.sda = -1, .scl = -1, .rst = -1, .vext = -1, .addr = 0},

    .button_gpio = -1,

    .battery = {.gpio = -1, .adc_channel = 0, .divider_factor = 0},

    /* All -1, same rationale as .battery/.radio above: the real charge and
     * VBUS-sense pins are wired by the nRF port's own battery backend
     * (nrf/shim/battery_t1000e.c), not this ESP-IDF-facing struct, which no
     * ESP driver reads for this board. */
    .charge = {.chrg_gpio = -1, .chrg_active_level = 0, .vbus_gpio = -1},

    .i2c_sda = -1,
    .i2c_scl = -1,

    .keyboard_int = -1,

    .trackball = {.up = -1, .down = -1, .left = -1, .right = -1, .center = -1},

    .touch = {.int_pin = -1, .rst_pin = -1, .i2c_addr = 0},

    .gps = {.tx = -1, .rx = -1, .baud = 0},

    .sdcard_cs = -1,

    .audio = {.i2s_ws = -1, .i2s_bck = -1, .i2s_dout = -1},
};

#endif /* BOARD_T1000E_H */
