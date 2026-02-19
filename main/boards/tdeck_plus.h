#ifndef BOARD_TDECK_PLUS_H
#define BOARD_TDECK_PLUS_H

#include "board_config.h"

static const bramble_board_config_t board_tdeck_plus = {
    .name = "LILYGO T-Deck Plus",
    .short_name = "tdeck_plus",
    .capabilities = BOARD_CAP_DISPLAY_ST7789 | BOARD_CAP_KEYBOARD |
                    BOARD_CAP_TRACKBALL | BOARD_CAP_GPS | BOARD_CAP_SDCARD |
                    BOARD_CAP_AUDIO | BOARD_CAP_BATTERY_ADC |
                    BOARD_CAP_SHARED_SPI | BOARD_CAP_PERIPHERAL_POWER,

    .peripheral_power_pin = 10,

    .spi = { .mosi = 41, .miso = 38, .sck = 40 },
    .spi_host = SPI2_HOST,
    .spi_max_transfer_sz = 320 * 240 * 2,  /* Full framebuffer DMA */

    .radio = { .cs = 9, .rst = 17, .busy = 13, .dio1 = 45 },
    .radio_osc = RADIO_OSC_CRYSTAL,
    .radio_tcxo_voltage = 0.0f,
    .radio_reg = RADIO_REG_LDO,

    .display_width = 320,
    .display_height = 240,
    .spi_display = { .cs = 12, .dc = 11, .backlight = 42 },

    .button_gpio = -1,  /* Trackball center handles this */

    .battery = { .gpio = 4, .adc_channel = 3, .divider_factor = 2 },

    .i2c_sda = 18,
    .i2c_scl = 8,
};

#endif /* BOARD_TDECK_PLUS_H */
