/*
 * Host coverage for the SX1262 LoRa bandwidth -> register-code mapping.
 *
 * Regression guard for issue #149: selecting 500 kHz silently ran the radio at
 * 125 kHz. The old path returned a kHz number (500) from a uint8_t helper and
 * passed it through a uint8_t parameter, truncating 500 to 244 so the driver's
 * switch fell through to the 125 kHz default. The mapping now lives in one pure
 * function, sx1262_bw_reg_from_hz(), which this suite exercises directly.
 */

#include "unity.h"
#include "sx1262.h"

void setUp(void) {}
void tearDown(void) {}

/* SX1262 SetModulationParams bandwidth register codes (datasheet 13.4.5). */
#define BW_125K_CODE 0x04
#define BW_250K_CODE 0x05
#define BW_500K_CODE 0x06

void test_125khz_maps_to_reg_0x04(void) {
    TEST_ASSERT_EQUAL_HEX8(BW_125K_CODE, sx1262_bw_reg_from_hz(125000));
}

void test_250khz_maps_to_reg_0x05(void) {
    TEST_ASSERT_EQUAL_HEX8(BW_250K_CODE, sx1262_bw_reg_from_hz(250000));
}

/* The defect itself: 500 kHz must program the 500 kHz code, and must not land
 * on the 125 kHz code the way the truncated path did. */
void test_500khz_maps_to_reg_0x06(void) {
    TEST_ASSERT_EQUAL_HEX8(BW_500K_CODE, sx1262_bw_reg_from_hz(500000));
    TEST_ASSERT_TRUE(sx1262_bw_reg_from_hz(500000) != sx1262_bw_reg_from_hz(125000));
}

/* All three user-selectable bandwidths must map to distinct codes, so a request
 * for one bandwidth can never silently run the chip at another. */
void test_all_three_bandwidths_distinct(void) {
    uint8_t c125 = sx1262_bw_reg_from_hz(125000);
    uint8_t c250 = sx1262_bw_reg_from_hz(250000);
    uint8_t c500 = sx1262_bw_reg_from_hz(500000);
    TEST_ASSERT_TRUE(c125 != c250);
    TEST_ASSERT_TRUE(c250 != c500);
    TEST_ASSERT_TRUE(c125 != c500);
}

/* Boundary behavior: values at and just below each rounded edge pick the
 * bandwidth at or above them, matching what the RPC layer validates. */
void test_boundaries(void) {
    TEST_ASSERT_EQUAL_HEX8(BW_125K_CODE, sx1262_bw_reg_from_hz(0));
    TEST_ASSERT_EQUAL_HEX8(BW_125K_CODE, sx1262_bw_reg_from_hz(125000));
    TEST_ASSERT_EQUAL_HEX8(BW_250K_CODE, sx1262_bw_reg_from_hz(125001));
    TEST_ASSERT_EQUAL_HEX8(BW_250K_CODE, sx1262_bw_reg_from_hz(250000));
    TEST_ASSERT_EQUAL_HEX8(BW_500K_CODE, sx1262_bw_reg_from_hz(250001));
    TEST_ASSERT_EQUAL_HEX8(BW_500K_CODE, sx1262_bw_reg_from_hz(500000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_125khz_maps_to_reg_0x04);
    RUN_TEST(test_250khz_maps_to_reg_0x05);
    RUN_TEST(test_500khz_maps_to_reg_0x06);
    RUN_TEST(test_all_three_bandwidths_distinct);
    RUN_TEST(test_boundaries);
    return UNITY_END();
}
