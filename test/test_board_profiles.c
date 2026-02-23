#include "unity.h"
#include "board_config.h"

void setUp(void) {}
void tearDown(void) {}

void test_heltec_v4_profile_identity_and_caps(void) {
    const bramble_board_config_t *cfg = board_get_config();
    TEST_ASSERT_NOT_NULL(cfg);

    TEST_ASSERT_EQUAL_STRING("Heltec WiFi LoRa 32 V4", cfg->name);
    TEST_ASSERT_EQUAL_STRING("heltec_v4", cfg->short_name);

    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_DISPLAY_SSD1306) != 0);
    TEST_ASSERT_TRUE((cfg->capabilities & BOARD_CAP_BATTERY_ADC) != 0);
}

void test_heltec_v4_profile_has_expected_radio_contract(void) {
    const bramble_board_config_t *cfg = board_get_config();
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
    const bramble_board_config_t *cfg = board_get_config();
    TEST_ASSERT_NOT_NULL(cfg);

    TEST_ASSERT_EQUAL_INT(128, cfg->display_width);
    TEST_ASSERT_EQUAL_INT(64, cfg->display_height);
    TEST_ASSERT_EQUAL_UINT8(0x3C, cfg->i2c_display.addr);

    TEST_ASSERT_EQUAL_INT(-1, cfg->gps.tx);
    TEST_ASSERT_EQUAL_INT(-1, cfg->gps.rx);
    TEST_ASSERT_EQUAL_INT(9600, cfg->gps.baud);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_heltec_v4_profile_identity_and_caps);
    RUN_TEST(test_heltec_v4_profile_has_expected_radio_contract);
    RUN_TEST(test_heltec_v4_profile_has_deterministic_optional_peripheral_defaults);
    return UNITY_END();
}
