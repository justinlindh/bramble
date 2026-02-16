#include "unity.h"
#include "../components/radio/radio_airtime.c"

void setUp(void) {}
void tearDown(void) {}

#define ASSERT_AIRTIME_NEAR(expected_ms, tolerance_ms, payload, sf, bw, cr) do { \
    uint32_t us = bramble_calculate_airtime_us(payload, sf, bw, cr); \
    uint32_t ms = us / 1000; \
    char msg[128]; \
    snprintf(msg, sizeof(msg), "Expected ~%u ms +/-%u, got %u ms", \
             (unsigned)(expected_ms), (unsigned)(tolerance_ms), (unsigned)ms); \
    TEST_ASSERT_MESSAGE(ms >= (expected_ms) - (tolerance_ms) && ms <= (expected_ms) + (tolerance_ms), msg); \
} while(0)

// Verified against Semtech AN1200.13 LoRa airtime calculator
// SF10, 125kHz, CR 4/6, preamble 12, explicit header, CRC on

void test_ack_22byte_sf10_125k_cr2(void) {
    // 22 bytes: preamble ~133ms + payload 38 symbols ~311ms = ~444ms
    ASSERT_AIRTIME_NEAR(444, 20, 22, 10, 125000, 2);
}

void test_beacon_36byte_sf10_125k_cr2(void) {
    // 36 bytes: ~591ms
    ASSERT_AIRTIME_NEAR(591, 20, 36, 10, 125000, 2);
}

void test_100byte_sf10_125k_cr2(void) {
    // 100 bytes: ~1230ms
    ASSERT_AIRTIME_NEAR(1230, 30, 100, 10, 125000, 2);
}

void test_200byte_sf10_125k_cr2(void) {
    // 200 bytes: ~2213ms
    ASSERT_AIRTIME_NEAR(2213, 30, 200, 10, 125000, 2);
}

void test_222byte_sf10_125k_cr2(void) {
    // 222 bytes: ~2410ms
    ASSERT_AIRTIME_NEAR(2410, 30, 222, 10, 125000, 2);
}

void test_100byte_sf8_250k_cr1(void) {
    // SF8, 250kHz, CR 4/5, preamble 8: ~153ms
    ASSERT_AIRTIME_NEAR(153, 20, 100, 8, 250000, 1);
}

void test_zero_payload(void) {
    // Edge case: 0 bytes
    uint32_t us = bramble_calculate_airtime_us(0, 10, 125000, 2);
    TEST_ASSERT_TRUE(us > 0);  // At minimum, preamble time
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ack_22byte_sf10_125k_cr2);
    RUN_TEST(test_beacon_36byte_sf10_125k_cr2);
    RUN_TEST(test_100byte_sf10_125k_cr2);
    RUN_TEST(test_200byte_sf10_125k_cr2);
    RUN_TEST(test_222byte_sf10_125k_cr2);
    RUN_TEST(test_100byte_sf8_250k_cr1);
    RUN_TEST(test_zero_payload);
    return UNITY_END();
}
