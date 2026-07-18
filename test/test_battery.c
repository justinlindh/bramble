#include "unity.h"
#include "../components/battery/battery_pct.c"

void setUp(void) {}
void tearDown(void) {}

/* Clamp behavior outside the modeled 3300-4200 mV span. */
void test_battery_pct_clamps(void) {
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_pct(4200));
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_pct(4500)); /* above full */
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(3300));
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(3000)); /* below cutoff */
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(0));
}

/* Each curve breakpoint maps to its declared percentage exactly. */
void test_battery_pct_breakpoints(void) {
    TEST_ASSERT_EQUAL_UINT8(90, battery_mv_to_pct(4060));
    TEST_ASSERT_EQUAL_UINT8(70, battery_mv_to_pct(3900));
    TEST_ASSERT_EQUAL_UINT8(50, battery_mv_to_pct(3800));
    TEST_ASSERT_EQUAL_UINT8(30, battery_mv_to_pct(3700));
    TEST_ASSERT_EQUAL_UINT8(15, battery_mv_to_pct(3600));
}

/* Interpolation is linear within a segment (midpoint of the top segment:
 * 4130 mV is halfway between 4060/90% and 4200/100%). */
void test_battery_pct_interpolates(void) {
    TEST_ASSERT_EQUAL_UINT8(95, battery_mv_to_pct(4130));
    /* Halfway between 3800/50% and 3900/70% -> 60%. */
    TEST_ASSERT_EQUAL_UINT8(60, battery_mv_to_pct(3850));
}

/* Percentage is monotonically non-decreasing in voltage across the span. */
void test_battery_pct_monotonic(void) {
    uint8_t prev = 0;
    for (uint32_t mv = 3300; mv <= 4200; mv += 10) {
        uint8_t pct = battery_mv_to_pct(mv);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(prev, pct);
        prev = pct;
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_battery_pct_clamps);
    RUN_TEST(test_battery_pct_breakpoints);
    RUN_TEST(test_battery_pct_interpolates);
    RUN_TEST(test_battery_pct_monotonic);
    return UNITY_END();
}
