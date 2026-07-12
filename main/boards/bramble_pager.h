#ifndef BOARD_BRAMBLE_PAGER_H
#define BOARD_BRAMBLE_PAGER_H

#include "board_config.h"

/*
 * Bramble Pager v1: ESP32-S3-WROOM-1 + NiceRF LoRa1262 (SX1262, TCXO) with a
 * shared SPI bus feeding the radio and a 2.13" SSD1680 e-paper panel.
 *
 * GNSS: ATGM336H-5N31 (CASIC AT6558, speaks PCAS config commands not Quectel
 *   PMTK; default NMEA 9600 8N1 so the firmware autobaud probe works unchanged).
 *   UART TX=35 (ESP to module RXD), RX=36 (module TXD to ESP). Power gate
 *   GNSS_EN=GPIO38 drives a P-FET high-side switch, active LOW (LOW = GNSS on);
 *   handled by the bramble_pager branch in components/gps/gps.c. GPIO35/36 are
 *   no longer reserved for an N8R8 octal-PSRAM option (dropped for GPS).
 *
 * E-paper (SSD1680, GDEY0213B74 2.13" 250x122): shares the SPI bus with
 *   the radio (BOARD_CAP_SHARED_SPI: board_init owns the bus + g_spi_mutex).
 *   CS=4, DC=5, RST=6 (RES#, active low), BUSY=7 (active HIGH here,
 *   opposite of UC8151). Driver: components/display/ssd1680_engine.c +
 *   ssd1680_io.c.
 *
 * Reserved for future drivers (not wired into this skeleton yet):
 *   Alert outputs: buzzer=15, vibra=16, LED=48.
 *   HMI buttons: DOWN=21 (RTC-capable, deep-sleep wake), UP=47 (BOOT/SELECT is
 *     button_gpio=0 below). Swapped in the rev B pre-fab pass: GPIO47 has no RTC
 *     alias, so the primary scroll/wake button (DOWN, middle front plunger SW403;
 *     BOOT is leftmost, UP rightmost) sits on
 *     GPIO21 instead. UP cannot wake from deep sleep; accepted trade.
 */
static const bramble_board_config_t board_bramble_pager = {
    .name = "Bramble Pager v1",
    .short_name = "bramble_pager",
    .capabilities = BOARD_CAP_BATTERY_ADC | BOARD_CAP_GPS | BOARD_CAP_DISPLAY_EPAPER |
                    BOARD_CAP_ALERTS | BOARD_CAP_SHARED_SPI,

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

#endif /* BOARD_BRAMBLE_PAGER_H */
