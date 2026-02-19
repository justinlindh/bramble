#ifndef BOARD_HELTEC_V3_H
#define BOARD_HELTEC_V3_H

#include "board_config.h"

static const bramble_board_config_t board_heltec_v3 = {
    .name = "Heltec WiFi LoRa 32 V3",
    .short_name = "heltec_v3",
    .capabilities = BOARD_CAP_DISPLAY_SSD1306 | BOARD_CAP_BATTERY_ADC,

    .peripheral_power_pin = -1,

    .spi = { .mosi = 10, .miso = 11, .sck = 9 },
    .spi_host = SPI2_HOST,
    .spi_max_transfer_sz = 256,

    .radio = { .cs = 8, .rst = 12, .busy = 13, .dio1 = 14 },
    .radio_osc = RADIO_OSC_TCXO_DIO3,
    .radio_tcxo_voltage = 1.7f,
    .radio_reg = RADIO_REG_DCDC,

    .display_width = 128,
    .display_height = 64,
    .i2c_display = { .sda = 17, .scl = 18, .rst = 21, .vext = 36, .addr = 0x3C },

    .button_gpio = 0,

    .battery = { .gpio = 1, .adc_channel = 0, .divider_factor = 2 },

    .i2c_sda = -1,
    .i2c_scl = -1,
};

#endif /* BOARD_HELTEC_V3_H */
