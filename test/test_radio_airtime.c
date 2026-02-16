#include "unity.h"
#include <stdio.h>
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

/*
 * All expected values computed with Semtech AN1200.13 formula:
 *   payloadSymbNb = 8 + max(ceil((8*PL - 4*SF + 28 + 16 - 20*IH) / (4*(SF - 2*DE))) * (CR+4), 0)
 *   IH=0 (explicit header), CRC=1, preamble=12 for SF>=9, 8 for SF<9
 */

/* 22-byte ACK at SF10/125k/CR2 (4/6): expect ~444ms */
void test_ack_22byte_sf10_125k_cr2(void) {
    ASSERT_AIRTIME_NEAR(444, 10, 22, 10, 125000, 2);
}

/* 36-byte beacon at SF10/125k/CR2: expect ~592ms */
void test_beacon_36byte_sf10_125k_cr2(void) {
    ASSERT_AIRTIME_NEAR(592, 10, 36, 10, 125000, 2);
}

/* 100-byte at SF10/125k/CR2: expect ~1231ms */
void test_100byte_sf10_125k_cr2(void) {
    ASSERT_AIRTIME_NEAR(1231, 10, 100, 10, 125000, 2);
}

/* 200-byte at SF10/125k/CR2: expect ~2214ms */
void test_200byte_sf10_125k_cr2(void) {
    ASSERT_AIRTIME_NEAR(2214, 10, 200, 10, 125000, 2);
}

/* 222-byte at SF10/125k/CR2: expect ~2411ms */
void test_222byte_sf10_125k_cr2(void) {
    ASSERT_AIRTIME_NEAR(2411, 10, 222, 10, 125000, 2);
}

/* 100-byte at SF8/250k/CR1 (4/5): expect ~154ms */
void test_100byte_sf8_250k_cr1(void) {
    ASSERT_AIRTIME_NEAR(154, 10, 100, 8, 250000, 1);
}

/* Verify monotonicity: larger payload → longer airtime */
void test_airtime_increases_with_payload(void) {
    uint32_t t22  = bramble_calculate_airtime_us(22, 10, 125000, 2);
    uint32_t t100 = bramble_calculate_airtime_us(100, 10, 125000, 2);
    uint32_t t200 = bramble_calculate_airtime_us(200, 10, 125000, 2);
    TEST_ASSERT_TRUE(t22 < t100);
    TEST_ASSERT_TRUE(t100 < t200);
}

/* Higher SF → longer airtime for same payload */
void test_airtime_increases_with_sf(void) {
    uint32_t t8  = bramble_calculate_airtime_us(50, 8, 125000, 1);
    uint32_t t10 = bramble_calculate_airtime_us(50, 10, 125000, 1);
    uint32_t t12 = bramble_calculate_airtime_us(50, 12, 125000, 1);
    TEST_ASSERT_TRUE(t8 < t10);
    TEST_ASSERT_TRUE(t10 < t12);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ack_22byte_sf10_125k_cr2);
    RUN_TEST(test_beacon_36byte_sf10_125k_cr2);
    RUN_TEST(test_100byte_sf10_125k_cr2);
    RUN_TEST(test_200byte_sf10_125k_cr2);
    RUN_TEST(test_222byte_sf10_125k_cr2);
    RUN_TEST(test_100byte_sf8_250k_cr1);
    RUN_TEST(test_airtime_increases_with_payload);
    RUN_TEST(test_airtime_increases_with_sf);
    return UNITY_END();
}
