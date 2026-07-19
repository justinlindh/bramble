#include "unity.h"
#include <stdio.h>
#include "../components/radio/radio_airtime.c"

void setUp(void) {}
void tearDown(void) {}

#define ASSERT_AIRTIME_NEAR(expected_ms, tolerance_ms, payload, sf, bw, cr)                        \
    do {                                                                                           \
        uint32_t us = bramble_calculate_airtime_us(payload, sf, bw, cr);                           \
        uint32_t ms = us / 1000;                                                                   \
        char msg[128];                                                                             \
        snprintf(msg, sizeof(msg), "Expected ~%u ms +/-%u, got %u ms", (unsigned)(expected_ms),    \
                 (unsigned)(tolerance_ms), (unsigned)ms);                                          \
        TEST_ASSERT_MESSAGE(                                                                       \
            ms >= (expected_ms) - (tolerance_ms) && ms <= (expected_ms) + (tolerance_ms), msg);    \
    } while (0)

/*
 * All expected values computed with Semtech AN1200.13 formula:
 *   payloadSymbNb = 8 + max(ceil((8*PL - 4*SF + 28 + 16 - 20*IH) / (4*(SF - 2*DE))) * (CR+4), 0)
 *   IH=0 (explicit header), CRC=1, preamble=12 for SF>=9, 8 for SF<9
 */

/* 22-byte ACK at SF10/125k/CR2 (4/6): expect ~444ms */
void test_ack_22byte_sf10_125k_cr2(void) { ASSERT_AIRTIME_NEAR(444, 10, 22, 10, 125000, 2); }

/* 36-byte beacon at SF10/125k/CR2: expect ~592ms */
void test_beacon_36byte_sf10_125k_cr2(void) { ASSERT_AIRTIME_NEAR(592, 10, 36, 10, 125000, 2); }

/* 100-byte at SF10/125k/CR2: expect ~1231ms */
void test_100byte_sf10_125k_cr2(void) { ASSERT_AIRTIME_NEAR(1231, 10, 100, 10, 125000, 2); }

/* 200-byte at SF10/125k/CR2: expect ~2214ms */
void test_200byte_sf10_125k_cr2(void) { ASSERT_AIRTIME_NEAR(2214, 10, 200, 10, 125000, 2); }

/* 222-byte at SF10/125k/CR2: expect ~2411ms */
void test_222byte_sf10_125k_cr2(void) { ASSERT_AIRTIME_NEAR(2411, 10, 222, 10, 125000, 2); }

/* 100-byte at SF8/250k/CR1 (4/5): expect ~154ms */
void test_100byte_sf8_250k_cr1(void) { ASSERT_AIRTIME_NEAR(154, 10, 100, 8, 250000, 1); }

/* Verify monotonicity: larger payload → longer airtime */
void test_airtime_increases_with_payload(void) {
    uint32_t t22 = bramble_calculate_airtime_us(22, 10, 125000, 2);
    uint32_t t100 = bramble_calculate_airtime_us(100, 10, 125000, 2);
    uint32_t t200 = bramble_calculate_airtime_us(200, 10, 125000, 2);
    TEST_ASSERT_TRUE(t22 < t100);
    TEST_ASSERT_TRUE(t100 < t200);
}

/* Higher SF → longer airtime for same payload */
void test_airtime_increases_with_sf(void) {
    uint32_t t8 = bramble_calculate_airtime_us(50, 8, 125000, 1);
    uint32_t t10 = bramble_calculate_airtime_us(50, 10, 125000, 1);
    uint32_t t12 = bramble_calculate_airtime_us(50, 12, 125000, 1);
    TEST_ASSERT_TRUE(t8 < t10);
    TEST_ASSERT_TRUE(t10 < t12);
}

/* ------------------------------------------------------------------ */
/*  Symbol time and CAD timeout derivation (issue #81)                  */
/* ------------------------------------------------------------------ */

/* t_sym = 2^sf / bw. At 125 kHz that is 1024 us at SF7, doubling per SF. */
void test_symbol_time_125k(void) {
    TEST_ASSERT_EQUAL_UINT32(1024, bramble_symbol_time_us(7, 125000));
    TEST_ASSERT_EQUAL_UINT32(2048, bramble_symbol_time_us(8, 125000));
    TEST_ASSERT_EQUAL_UINT32(4096, bramble_symbol_time_us(9, 125000));
    TEST_ASSERT_EQUAL_UINT32(8192, bramble_symbol_time_us(10, 125000));
    TEST_ASSERT_EQUAL_UINT32(16384, bramble_symbol_time_us(11, 125000));
    TEST_ASSERT_EQUAL_UINT32(32768, bramble_symbol_time_us(12, 125000));
}

/* Doubling the bandwidth halves the symbol time. */
void test_symbol_time_scales_with_bandwidth(void) {
    TEST_ASSERT_EQUAL_UINT32(4096, bramble_symbol_time_us(10, 250000));
    TEST_ASSERT_EQUAL_UINT32(2048, bramble_symbol_time_us(10, 500000));
    TEST_ASSERT_EQUAL_UINT32(16384, bramble_symbol_time_us(10, 62500));
}

/* Out-of-range inputs clamp instead of producing nonsense. */
void test_symbol_time_clamps_inputs(void) {
    TEST_ASSERT_EQUAL_UINT32(bramble_symbol_time_us(12, 125000),
                             bramble_symbol_time_us(30, 125000));
    TEST_ASSERT_EQUAL_UINT32(bramble_symbol_time_us(5, 125000), bramble_symbol_time_us(0, 125000));
    /* bw_hz == 0 falls back to 125 kHz rather than dividing by zero. */
    TEST_ASSERT_EQUAL_UINT32(bramble_symbol_time_us(10, 125000), bramble_symbol_time_us(10, 0));
}

/*
 * The driver's CAD budget at the shipped cadSymbolNum register value 2,
 * which the SX1262 encodes as 4 symbols (2^2), at 125 kHz:
 *   budget_ms = ceil(4 * t_sym_us * 2 / 1000) + 10, floored at 50.
 */
void test_cad_timeout_125k_per_sf(void) {
    TEST_ASSERT_EQUAL_UINT32(50, bramble_cad_timeout_ms(7, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG));
    TEST_ASSERT_EQUAL_UINT32(50, bramble_cad_timeout_ms(8, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG));
    TEST_ASSERT_EQUAL_UINT32(50, bramble_cad_timeout_ms(9, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG));
    TEST_ASSERT_EQUAL_UINT32(76, bramble_cad_timeout_ms(10, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG));
    TEST_ASSERT_EQUAL_UINT32(142, bramble_cad_timeout_ms(11, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG));
    TEST_ASSERT_EQUAL_UINT32(273, bramble_cad_timeout_ms(12, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG));
}

/* The regression this fixes: the budget must exceed the raw CAD duration at
 * every SF, which the old fixed 50 ms did not do above SF9. */
void test_cad_timeout_exceeds_cad_duration(void) {
    for (uint8_t sf = 7; sf <= 12; sf++) {
        uint32_t cad_ms = (4u * bramble_symbol_time_us(sf, 125000)) / 1000u;
        uint32_t budget = bramble_cad_timeout_ms(sf, 125000, BRAMBLE_CAD_SYMBOL_NUM_REG);
        char msg[96];
        snprintf(msg, sizeof(msg), "SF%u: budget %u ms must exceed CAD %u ms", (unsigned)sf,
                 (unsigned)budget, (unsigned)cad_ms);
        TEST_ASSERT_TRUE_MESSAGE(budget > cad_ms, msg);
    }
}

/* More CAD symbols means a longer budget, and the register field saturates at
 * 4 (16 symbols) so an out-of-range value cannot shrink the wait. */
void test_cad_timeout_scales_with_symbol_count(void) {
    uint32_t s4 = bramble_cad_timeout_ms(12, 125000, 2);
    uint32_t s8 = bramble_cad_timeout_ms(12, 125000, 3);
    uint32_t s16 = bramble_cad_timeout_ms(12, 125000, 4);
    TEST_ASSERT_TRUE(s4 < s8);
    TEST_ASSERT_TRUE(s8 < s16);
    TEST_ASSERT_EQUAL_UINT32(s16, bramble_cad_timeout_ms(12, 125000, 200));
}

/* Wider bandwidth shortens the budget until the floor takes over. */
void test_cad_timeout_floor(void) {
    TEST_ASSERT_EQUAL_UINT32(BRAMBLE_CAD_TIMEOUT_MIN_MS,
                             bramble_cad_timeout_ms(7, 500000, BRAMBLE_CAD_SYMBOL_NUM_REG));
    TEST_ASSERT_EQUAL_UINT32(142, bramble_cad_timeout_ms(12, 250000, BRAMBLE_CAD_SYMBOL_NUM_REG));
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
    RUN_TEST(test_symbol_time_125k);
    RUN_TEST(test_symbol_time_scales_with_bandwidth);
    RUN_TEST(test_symbol_time_clamps_inputs);
    RUN_TEST(test_cad_timeout_125k_per_sf);
    RUN_TEST(test_cad_timeout_exceeds_cad_duration);
    RUN_TEST(test_cad_timeout_scales_with_symbol_count);
    RUN_TEST(test_cad_timeout_floor);
    return UNITY_END();
}
