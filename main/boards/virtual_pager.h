#ifndef BOARD_VIRTUAL_PAGER_H
#define BOARD_VIRTUAL_PAGER_H

#include "board_config.h"

/*
 * Bramble Virtual Pager: the emulator's board profile for the IDF linux
 * target. Single source of truth is boards/bramble_pager.h, the real
 * hardware profile; every field here is copied verbatim from it so
 * firmware code paths and pin numbers in logs stay comparable across the
 * real device and the emulated node. Only the capability mask differs, by
 * the addition of BOARD_CAP_VIRTUAL, which marks this profile as backing
 * no real hardware.
 *
 * See boards/bramble_pager.h for pin rationale (GNSS module, radio, etc).
 * test/test_board_profiles.c enforces this parity: it fails if the two
 * profiles ever drift apart except for BOARD_CAP_VIRTUAL.
 */
static const bramble_board_config_t board_virtual_pager = {
    .name = "Bramble Virtual Pager",
    .short_name = "virtual_pager",
    .capabilities = BOARD_CAP_BATTERY_ADC | BOARD_CAP_GPS | BOARD_CAP_DISPLAY_EPAPER |
                    BOARD_CAP_SHARED_SPI | BOARD_CAP_VIRTUAL,

    .peripheral_power_pin = -1,

    .spi = {.mosi = 10, .miso = 11, .sck = 9},
    .spi_host = SPI2_HOST,
    .spi_max_transfer_sz = 256,

    .radio = {.cs = 8, .rst = 12, .busy = 13, .dio1 = 14},
    .radio_osc = RADIO_OSC_TCXO_DIO3,
    .radio_tcxo_voltage = 2.7f,
    .radio_reg = RADIO_REG_DCDC,
    .radio_dio2_rf_switch = true,

    /* SSD1680 e-paper, landscape as the firmware sees it */
    .display_width = 250,
    .display_height = 122,
    .epd_display = {.cs = 4, .dc = 5, .rst = 6, .busy = 7},

    .button_gpio = 0,

    .battery = {.gpio = 1, .adc_channel = 0, .divider_factor = 2},

    /* Debug header I2C; 10k pullups always fitted on board. */
    .i2c_sda = 17,
    .i2c_scl = 18,

    .keyboard_int = -1,

    .trackball = {.up = -1, .down = -1, .left = -1, .right = -1, .center = -1},

    .touch = {.int_pin = -1, .rst_pin = -1, .i2c_addr = 0},

    /* ATGM336H GNSS: tx = ESP transmit to module RXD, rx = ESP receive from
       module TXD. Power gate on GPIO38 (active LOW) lives in gps.c. */
    .gps = {.tx = 35, .rx = 36, .baud = 9600},

    .sdcard_cs = -1,

    .audio = {.i2s_ws = -1, .i2s_bck = -1, .i2s_dout = -1},
};

#endif /* BOARD_VIRTUAL_PAGER_H */
