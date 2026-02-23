#ifndef BOARD_HELTEC_V4_H
#define BOARD_HELTEC_V4_H

#include "board_config.h"

static const bramble_board_config_t board_heltec_v4 = {
    .name = "Heltec WiFi LoRa 32 V4",
    .short_name = "heltec_v4",
    .capabilities = BOARD_CAP_DISPLAY_SSD1306 | BOARD_CAP_BATTERY_ADC |
                    BOARD_CAP_GPS,

    .peripheral_power_pin = -1,

    /* TODO(heltec-v4): Replace placeholder pins with schematic-confirmed values. */
    .spi = { .mosi = 10, .miso = 11, .sck = 9 },
    .spi_host = SPI2_HOST,
    .spi_max_transfer_sz = 256,

    .radio = { .cs = 8, .rst = 12, .busy = 13, .dio1 = 14 },
    .radio_osc = RADIO_OSC_TCXO_DIO3,
    .radio_tcxo_voltage = 1.8f,
    .radio_reg = RADIO_REG_DCDC,

    .display_width = 128,
    .display_height = 64,
    .i2c_display = { .sda = 17, .scl = 18, .rst = 21, .vext = 36, .addr = 0x3C },

    .button_gpio = 0,

    .battery = { .gpio = 1, .adc_channel = 0, .divider_factor = 2 },

    .i2c_sda = -1,
    .i2c_scl = -1,

    .keyboard_int = -1,

    .trackball = { .up = -1, .down = -1, .left = -1, .right = -1, .center = -1 },

    .touch = { .int_pin = -1, .rst_pin = -1, .i2c_addr = 0 },

    /* TODO(heltec-v4): Confirm GNSS UART mapping from V4 + L76K documentation. */
    .gps = { .tx = -1, .rx = -1, .baud = 9600 },

    .sdcard_cs = -1,

    .audio = { .i2s_ws = -1, .i2s_bck = -1, .i2s_dout = -1 },
};

#endif /* BOARD_HELTEC_V4_H */
