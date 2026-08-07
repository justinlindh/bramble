/*
 * T1000-E battery conversion math (nrf/shim/include/battery_t1000e_conv.h):
 * the exact raw-code-to-cell-millivolts arithmetic the device runs, plus its
 * interaction with the shared discharge curve. The anchor test pins the one
 * hardware-verified point: the 2026-08-06 bench probe read 3920 mV / 72 pct
 * on a charging cell with the P1.06 rail driven, so the code path that
 * produced those numbers must keep producing them.
 */
#include "unity.h"

#include "../components/battery/battery_pct.c"
#include "battery_t1000e_conv.h"

void setUp(void) {}
void tearDown(void) {}

/* Zero and negative raw codes (rail noise near 0V) clamp to 0 mV. */
void test_raw_clamps_low(void) {
    TEST_ASSERT_EQUAL_UINT32(0, battery_t1000e_raw_to_pin_mv(0));
    TEST_ASSERT_EQUAL_UINT32(0, battery_t1000e_raw_to_pin_mv(-1));
    TEST_ASSERT_EQUAL_UINT32(0, battery_t1000e_raw_to_pin_mv(-32768));
}

/* Full scale: the top 14-bit code maps just under 3000 mV at the pin, and
 * codes beyond the peripheral's range clamp instead of overflowing. */
void test_raw_full_scale(void) {
    TEST_ASSERT_EQUAL_UINT32(2999, battery_t1000e_raw_to_pin_mv(16383));
    TEST_ASSERT_EQUAL_UINT32(2999, battery_t1000e_raw_to_pin_mv(16384));
    TEST_ASSERT_EQUAL_UINT32(2999, battery_t1000e_raw_to_pin_mv(32767));
}

/* Midpoint sanity: half the code range is half of full scale. */
void test_raw_midpoint(void) { TEST_ASSERT_EQUAL_UINT32(1500, battery_t1000e_raw_to_pin_mv(8192)); }

/* raw -> pin mv is monotonically non-decreasing across the code range. */
void test_raw_monotonic(void) {
    uint32_t prev = 0;
    for (int32_t raw = 0; raw <= 16383; raw += 7) {
        uint32_t mv = battery_t1000e_raw_to_pin_mv((int16_t)raw);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(prev, mv);
        prev = mv;
    }
}

/* The divider doubles pin mv to cell mv. */
void test_divider(void) {
    TEST_ASSERT_EQUAL_UINT32(0, battery_t1000e_pin_to_vbat_mv(0));
    TEST_ASSERT_EQUAL_UINT32(3920, battery_t1000e_pin_to_vbat_mv(1960));
    TEST_ASSERT_EQUAL_UINT32(5998, battery_t1000e_pin_to_vbat_mv(2999));
}

/* Bench anchor: a raw code corresponding to ~1.96 V at the pin must come
 * out as the probe's 3918-3920 mV cell reading, and that voltage must map
 * to the 72 pct the bench observed. 10704 = round(1960 * 16384 / 3000). */
void test_bench_anchor_3920mv_72pct(void) {
    uint32_t pin_mv = battery_t1000e_raw_to_pin_mv(10704);
    uint32_t cell_mv = battery_t1000e_pin_to_vbat_mv(pin_mv);
    TEST_ASSERT_UINT32_WITHIN(2, 3918, cell_mv);
    TEST_ASSERT_EQUAL_UINT8(72, battery_mv_to_pct(3920));
}

/* The dead-divider signature (rail gated off: single-digit mV at the pin)
 * must land at 0 pct, never a plausible number: this is the confidently
 * wrong reading the rail gate exists to prevent. */
void test_dead_divider_reads_zero(void) {
    uint32_t pin_mv = battery_t1000e_raw_to_pin_mv(11); /* ~2 mV */
    TEST_ASSERT_EQUAL_UINT32(2, pin_mv);
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(battery_t1000e_pin_to_vbat_mv(pin_mv)));
}

/* Average: all valid, mixed validity (failed samples excluded, not folded
 * in as zeros), and the all-failed case reporting 0. */
void test_average(void) {
    uint32_t s1[4] = {100, 200, 300, 400};
    bool all[4] = {true, true, true, true};
    TEST_ASSERT_EQUAL_UINT32(250, battery_t1000e_average_mv(s1, all, 4));

    uint32_t s2[4] = {1960, 0, 1970, 0};
    bool mixed[4] = {true, false, true, false};
    TEST_ASSERT_EQUAL_UINT32(1965, battery_t1000e_average_mv(s2, mixed, 4));

    bool none[4] = {false, false, false, false};
    TEST_ASSERT_EQUAL_UINT32(0, battery_t1000e_average_mv(s2, none, 4));
}

/* The survival latch permits the rail drive for every stored state except a
 * recorded died-mid-window verdict; unknown or corrupt bytes count as
 * untried rather than disabling the feature on garbage. */
void test_probe_latch_decision(void) {
    TEST_ASSERT_TRUE(battery_t1000e_vbat_allowed(BATTERY_T1000E_PROBE_UNTRIED));
    TEST_ASSERT_FALSE(battery_t1000e_vbat_allowed(BATTERY_T1000E_PROBE_ATTEMPTING));
    TEST_ASSERT_TRUE(battery_t1000e_vbat_allowed(BATTERY_T1000E_PROBE_PROVEN));
    TEST_ASSERT_TRUE(battery_t1000e_vbat_allowed(0x7f)); /* corrupt byte */
    TEST_ASSERT_TRUE(battery_t1000e_vbat_allowed(0xff)); /* erased flash */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_raw_clamps_low);
    RUN_TEST(test_raw_full_scale);
    RUN_TEST(test_raw_midpoint);
    RUN_TEST(test_raw_monotonic);
    RUN_TEST(test_divider);
    RUN_TEST(test_bench_anchor_3920mv_72pct);
    RUN_TEST(test_dead_divider_reads_zero);
    RUN_TEST(test_average);
    RUN_TEST(test_probe_latch_decision);
    return UNITY_END();
}
