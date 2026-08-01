#include "unity.h"
#include "board_config.h"
#include "boards/heltec_v3.h"
#include "boards/bramble_pager.h"
#include "boards/virtual_pager.h"

void setUp(void) {}
void tearDown(void) {}

void test_heltec_v4_profile_identity_and_caps(void) {
    const bramble_board_config_t* cfg = board_get_config();
    TEST_ASSERT_NOT_NULL(cfg);

    TEST_ASSERT_EQUAL_STRING("Heltec WiFi LoRa 32 V4", cfg->name);
    TEST_ASSERT_EQUAL_STRING("heltec_v4", cfg->short_name);

    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_DISPLAY_SSD1306) != 0);
    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_BATTERY_ADC) != 0);
}

void test_heltec_v4_profile_has_expected_radio_contract(void) {
    const bramble_board_config_t* cfg = board_get_config();
    TEST_ASSERT_NOT_NULL(cfg);

    TEST_ASSERT_EQUAL_INT(10, cfg->spi.mosi);
    TEST_ASSERT_EQUAL_INT(11, cfg->spi.miso);
    TEST_ASSERT_EQUAL_INT(9, cfg->spi.sck);

    TEST_ASSERT_EQUAL_INT(8, cfg->radio.cs);
    TEST_ASSERT_EQUAL_INT(12, cfg->radio.rst);
    TEST_ASSERT_EQUAL_INT(13, cfg->radio.busy);
    TEST_ASSERT_EQUAL_INT(14, cfg->radio.dio1);

    TEST_ASSERT_EQUAL_INT(RADIO_OSC_TCXO_DIO3, cfg->radio_osc);
    TEST_ASSERT_EQUAL_INT(RADIO_REG_DCDC, cfg->radio_reg);
}

void test_heltec_v4_profile_has_deterministic_optional_peripheral_defaults(void) {
    const bramble_board_config_t* cfg = board_get_config();
    TEST_ASSERT_NOT_NULL(cfg);

    TEST_ASSERT_EQUAL_INT(128, cfg->display_width);
    TEST_ASSERT_EQUAL_INT(64, cfg->display_height);
    TEST_ASSERT_EQUAL_UINT8(0x3C, cfg->i2c_display.addr);

    TEST_ASSERT_EQUAL_INT(38, cfg->gps.tx);
    TEST_ASSERT_EQUAL_INT(39, cfg->gps.rx);
    TEST_ASSERT_EQUAL_INT(9600, cfg->gps.baud);
}

void test_heltec_v3_profile_does_not_use_dio2_rf_switch(void) {
    TEST_ASSERT_FALSE(board_heltec_v3.radio_dio2_rf_switch);
}

void test_bramble_pager_profile_identity_and_caps(void) {
    const bramble_board_config_t* cfg = &board_bramble_pager;

    TEST_ASSERT_EQUAL_STRING("Bramble Pager v1", cfg->name);
    TEST_ASSERT_EQUAL_STRING("bramble_pager", cfg->short_name);

    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_BATTERY_ADC) != 0);
    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_GPS) != 0);
    TEST_ASSERT_FALSE((cfg->capabilities & BOARD_CAP_DISPLAY_SSD1306) != 0);
}

void test_bramble_pager_profile_has_expected_radio_contract(void) {
    const bramble_board_config_t* cfg = &board_bramble_pager;

    TEST_ASSERT_EQUAL_INT(10, cfg->spi.mosi);
    TEST_ASSERT_EQUAL_INT(11, cfg->spi.miso);
    TEST_ASSERT_EQUAL_INT(9, cfg->spi.sck);

    TEST_ASSERT_EQUAL_INT(8, cfg->radio.cs);
    TEST_ASSERT_EQUAL_INT(12, cfg->radio.rst);
    TEST_ASSERT_EQUAL_INT(13, cfg->radio.busy);
    TEST_ASSERT_EQUAL_INT(14, cfg->radio.dio1);

    TEST_ASSERT_EQUAL_INT(RADIO_OSC_TCXO_DIO3, cfg->radio_osc);
    TEST_ASSERT_EQUAL_FLOAT(2.7f, cfg->radio_tcxo_voltage);
    TEST_ASSERT_EQUAL_INT(RADIO_REG_DCDC, cfg->radio_reg);
    TEST_ASSERT_TRUE(cfg->radio_dio2_rf_switch);
}

void test_bramble_pager_profile_has_epd_display_contract(void) {
    const bramble_board_config_t* cfg = &board_bramble_pager;

    /* SSD1680 e-paper on the shared radio SPI bus (board_init owns the
     * bus and g_spi_mutex when BOARD_CAP_SHARED_SPI is set). */
    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_DISPLAY_EPAPER) != 0);
    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_SHARED_SPI) != 0);

    TEST_ASSERT_EQUAL_INT(250, cfg->display_width);
    TEST_ASSERT_EQUAL_INT(122, cfg->display_height);

    TEST_ASSERT_EQUAL_INT(4, cfg->epd_display.cs);
    TEST_ASSERT_EQUAL_INT(5, cfg->epd_display.dc);
    TEST_ASSERT_EQUAL_INT(6, cfg->epd_display.rst);
    TEST_ASSERT_EQUAL_INT(7, cfg->epd_display.busy);
}

void test_bramble_pager_profile_has_expected_peripheral_pins(void) {
    const bramble_board_config_t* cfg = &board_bramble_pager;

    TEST_ASSERT_EQUAL_INT(0, cfg->button_gpio);

    TEST_ASSERT_EQUAL_INT(1, cfg->battery.gpio);
    TEST_ASSERT_EQUAL_INT(0, cfg->battery.adc_channel);
    TEST_ASSERT_EQUAL_INT(2, cfg->battery.divider_factor);

    /* No board has a charge-detect pin wired yet (wave 2): every profile
     * ships {-1, 0, -1} explicitly, since the struct's designated
     * initializers mean an omitted .charge would zero-init to
     * {chrg_gpio=0, ...}, silently misreading GPIO0 as a charge pin. */
    TEST_ASSERT_EQUAL_INT(-1, cfg->charge.chrg_gpio);
    TEST_ASSERT_EQUAL_INT(0, cfg->charge.chrg_active_level);
    TEST_ASSERT_EQUAL_INT(-1, cfg->charge.vbus_gpio);

    TEST_ASSERT_EQUAL_INT(17, cfg->i2c_sda);
    TEST_ASSERT_EQUAL_INT(18, cfg->i2c_scl);

    TEST_ASSERT_EQUAL_INT(35, cfg->gps.tx);
    TEST_ASSERT_EQUAL_INT(36, cfg->gps.rx);
    TEST_ASSERT_EQUAL_INT(9600, cfg->gps.baud);
}

/*
 * Parity test: the virtual pager profile is derived from the real
 * bramble_pager profile (single source of truth is the real header), so
 * every capability flag and every pin must match exactly, except the
 * virtual profile also carries the BOARD_CAP_VIRTUAL marker flag.
 */
void test_virtual_pager_profile_has_bramble_pager_caps_plus_virtual_marker(void) {
    const bramble_board_config_t* real = &board_bramble_pager;
    const bramble_board_config_t* virt = &board_virtual_pager;

    TEST_ASSERT_EQUAL_UINT32(real->capabilities | BOARD_CAP_VIRTUAL, virt->capabilities);
    TEST_ASSERT_TRUE((virt->capabilities & BOARD_CAP_VIRTUAL) != 0);
    TEST_ASSERT_FALSE((real->capabilities & BOARD_CAP_VIRTUAL) != 0);
}

void test_virtual_pager_profile_matches_bramble_pager_pins(void) {
    const bramble_board_config_t* real = &board_bramble_pager;
    const bramble_board_config_t* virt = &board_virtual_pager;

    TEST_ASSERT_EQUAL_INT(real->peripheral_power_pin, virt->peripheral_power_pin);

    TEST_ASSERT_EQUAL_INT(real->spi.mosi, virt->spi.mosi);
    TEST_ASSERT_EQUAL_INT(real->spi.miso, virt->spi.miso);
    TEST_ASSERT_EQUAL_INT(real->spi.sck, virt->spi.sck);
    TEST_ASSERT_EQUAL_INT(real->spi_host, virt->spi_host);
    TEST_ASSERT_EQUAL_INT(real->spi_max_transfer_sz, virt->spi_max_transfer_sz);

    TEST_ASSERT_EQUAL_INT(real->radio.cs, virt->radio.cs);
    TEST_ASSERT_EQUAL_INT(real->radio.rst, virt->radio.rst);
    TEST_ASSERT_EQUAL_INT(real->radio.busy, virt->radio.busy);
    TEST_ASSERT_EQUAL_INT(real->radio.dio1, virt->radio.dio1);
    TEST_ASSERT_EQUAL_INT(real->radio_osc, virt->radio_osc);
    TEST_ASSERT_EQUAL_FLOAT(real->radio_tcxo_voltage, virt->radio_tcxo_voltage);
    TEST_ASSERT_EQUAL_INT(real->radio_reg, virt->radio_reg);
    TEST_ASSERT_EQUAL_INT(real->radio_dio2_rf_switch, virt->radio_dio2_rf_switch);

    TEST_ASSERT_EQUAL_INT(real->button_gpio, virt->button_gpio);

    TEST_ASSERT_EQUAL_INT(real->battery.gpio, virt->battery.gpio);
    TEST_ASSERT_EQUAL_INT(real->battery.adc_channel, virt->battery.adc_channel);
    TEST_ASSERT_EQUAL_INT(real->battery.divider_factor, virt->battery.divider_factor);

    TEST_ASSERT_EQUAL_INT(real->charge.chrg_gpio, virt->charge.chrg_gpio);
    TEST_ASSERT_EQUAL_INT(real->charge.chrg_active_level, virt->charge.chrg_active_level);
    TEST_ASSERT_EQUAL_INT(real->charge.vbus_gpio, virt->charge.vbus_gpio);

    TEST_ASSERT_EQUAL_INT(real->i2c_sda, virt->i2c_sda);
    TEST_ASSERT_EQUAL_INT(real->i2c_scl, virt->i2c_scl);

    TEST_ASSERT_EQUAL_INT(real->keyboard_int, virt->keyboard_int);

    TEST_ASSERT_EQUAL_INT(real->trackball.up, virt->trackball.up);
    TEST_ASSERT_EQUAL_INT(real->trackball.down, virt->trackball.down);
    TEST_ASSERT_EQUAL_INT(real->trackball.left, virt->trackball.left);
    TEST_ASSERT_EQUAL_INT(real->trackball.right, virt->trackball.right);
    TEST_ASSERT_EQUAL_INT(real->trackball.center, virt->trackball.center);

    TEST_ASSERT_EQUAL_INT(real->touch.int_pin, virt->touch.int_pin);
    TEST_ASSERT_EQUAL_INT(real->touch.rst_pin, virt->touch.rst_pin);
    TEST_ASSERT_EQUAL_UINT8(real->touch.i2c_addr, virt->touch.i2c_addr);

    TEST_ASSERT_EQUAL_INT(real->gps.tx, virt->gps.tx);
    TEST_ASSERT_EQUAL_INT(real->gps.rx, virt->gps.rx);
    TEST_ASSERT_EQUAL_INT(real->gps.baud, virt->gps.baud);

    TEST_ASSERT_EQUAL_INT(real->sdcard_cs, virt->sdcard_cs);

    TEST_ASSERT_EQUAL_INT(real->audio.i2s_ws, virt->audio.i2s_ws);
    TEST_ASSERT_EQUAL_INT(real->audio.i2s_bck, virt->audio.i2s_bck);
    TEST_ASSERT_EQUAL_INT(real->audio.i2s_dout, virt->audio.i2s_dout);

    TEST_ASSERT_EQUAL_INT(real->display_width, virt->display_width);
    TEST_ASSERT_EQUAL_INT(real->display_height, virt->display_height);

    TEST_ASSERT_EQUAL_INT(real->epd_display.cs, virt->epd_display.cs);
    TEST_ASSERT_EQUAL_INT(real->epd_display.dc, virt->epd_display.dc);
    TEST_ASSERT_EQUAL_INT(real->epd_display.rst, virt->epd_display.rst);
    TEST_ASSERT_EQUAL_INT(real->epd_display.busy, virt->epd_display.busy);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_heltec_v4_profile_identity_and_caps);
    RUN_TEST(test_heltec_v4_profile_has_expected_radio_contract);
    RUN_TEST(test_heltec_v4_profile_has_deterministic_optional_peripheral_defaults);
    RUN_TEST(test_heltec_v3_profile_does_not_use_dio2_rf_switch);
    RUN_TEST(test_bramble_pager_profile_identity_and_caps);
    RUN_TEST(test_bramble_pager_profile_has_epd_display_contract);
    RUN_TEST(test_bramble_pager_profile_has_expected_radio_contract);
    RUN_TEST(test_bramble_pager_profile_has_expected_peripheral_pins);
    RUN_TEST(test_virtual_pager_profile_has_bramble_pager_caps_plus_virtual_marker);
    RUN_TEST(test_virtual_pager_profile_matches_bramble_pager_pins);
    return UNITY_END();
}
