#ifndef BOARD_WIO_WM1110_H
#define BOARD_WIO_WM1110_H

#include "board_config.h"

/*
 * Seeed Wio-WM1110 Dev Kit: the nRF52840 target's board profile (bare-metal
 * FreeRTOS, LR1110 radio; the SenseCAP T1000-E will get its own profile when
 * it becomes a supported device). Without this profile the nRF build fell
 * through main/board.c's default and reported itself to every RPC client as
 * a heltec_v3, which is exactly the kind of claim the honesty conventions
 * exist to prevent.
 *
 * The pin fields are deliberately all -1: on this target the real pin
 * mapping is owned by the port (nrf/boards/wio_wm1110_devkit.h) and the
 * drivers that consume THESE fields are the ESP-IDF ones, which do not build
 * for nRF. This struct exists for identity (name, short_name) and the
 * capability mask, which correctly advertises: no display, no keyboard, no
 * battery ADC wired yet, and GNSS not brought up until P3.
 */
static const bramble_board_config_t board_wio_wm1110 = {
    .name = "Seeed Wio-WM1110 Dev Kit",
    .short_name = "wio_wm1110",
    .capabilities = 0,

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

    .i2c_sda = -1,
    .i2c_scl = -1,

    .keyboard_int = -1,

    .trackball = {.up = -1, .down = -1, .left = -1, .right = -1, .center = -1},

    .touch = {.int_pin = -1, .rst_pin = -1, .i2c_addr = 0},

    .gps = {.tx = -1, .rx = -1, .baud = 0},

    .sdcard_cs = -1,

    .audio = {.i2s_ws = -1, .i2s_bck = -1, .i2s_dout = -1},
};

#endif /* BOARD_WIO_WM1110_H */
