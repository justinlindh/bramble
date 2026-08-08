#ifndef BOARD_VIRTUAL_HELTEC_H
#define BOARD_VIRTUAL_HELTEC_H

#include "board_config.h"

/*
 * Bramble Virtual Heltec: the emulator's board profile for a Heltec WiFi LoRa
 * 32 V4 (ESP32-S3 + SX1262 + GNSS, SSD1306 128x64 OLED) on the IDF linux
 * target. It mirrors boards/heltec_v4.h, the real profile, field for field;
 * only the capability mask differs, by the addition of BOARD_CAP_VIRTUAL, which
 * marks this profile as backing no real hardware. Same relationship
 * virtual_pager.h has to bramble_pager.h, so firmware code paths and pin
 * numbers in logs stay comparable between the real Heltec and the emulated
 * node. V4 (not V3) is mirrored so the emulated node has GPS, which makes the
 * GPS text-UI screen reachable in the emulator the same way the real board has
 * it.
 *
 * The emulated node has no real I2C bus: components/display/display_virt_oled.c
 * implements the display.h contract on the linux target and streams the 128x64
 * framebuffer to the emu-link broker instead of driving the panel.
 */
static const bramble_board_config_t board_virtual_heltec = {
    .name = "Bramble Virtual Heltec",
    .short_name = "virtual_heltec",
    .capabilities =
        BOARD_CAP_DISPLAY_SSD1306 | BOARD_CAP_BATTERY_ADC | BOARD_CAP_GPS | BOARD_CAP_VIRTUAL,

    .peripheral_power_pin = -1,

    .spi = {.mosi = 10, .miso = 11, .sck = 9},
    .spi_host = SPI2_HOST,
    .spi_max_transfer_sz = 256,

    .radio = {.cs = 8, .rst = 12, .busy = 13, .dio1 = 14},
    .radio_osc = RADIO_OSC_TCXO_DIO3,
    .radio_tcxo_voltage = 1.8f,
    .radio_reg = RADIO_REG_DCDC,

    /* SSD1306 OLED, 128x64 */
    .display_width = 128,
    .display_height = 64,
    .i2c_display = {.sda = 17, .scl = 18, .rst = 21, .vext = 36, .addr = 0x3C},

    .button_gpio = 0,

    .battery = {.gpio = 1, .adc_channel = 0, .divider_factor = 5},
    .charge = {.chrg_gpio = -1, .chrg_active_level = 0, .vbus_gpio = -1},

    .i2c_sda = -1,
    .i2c_scl = -1,

    .keyboard_int = -1,

    .trackball = {.up = -1, .down = -1, .left = -1, .right = -1, .center = -1},

    .touch = {.int_pin = -1, .rst_pin = -1, .i2c_addr = 0},

    /* Heltec V4 GNSS mapping (mirrors heltec_v4.h). */
    .gps = {.tx = 38, .rx = 39, .baud = 9600},

    .sdcard_cs = -1,

    .audio = {.i2s_ws = -1, .i2s_bck = -1, .i2s_dout = -1},
};

#endif /* BOARD_VIRTUAL_HELTEC_H */
