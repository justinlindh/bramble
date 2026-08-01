#include "unity.h"
#include "../components/battery/battery_pct.c"
#include "../components/battery/battery_helpers.c"

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

/* ── battery_average_mv (wave 2) ─────────────────────────────────────── */

void test_battery_average_mv_simple_mean(void) {
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT] = {4790, 4800, 4805, 4795, 4800, 4810, 4790, 4800};
    /* Sum = 38390, / 8 = 4798.75 -> 4798 (integer truncation). */
    TEST_ASSERT_EQUAL_UINT32(4798, battery_average_mv(samples, BATTERY_AVG_SAMPLE_COUNT));
}

void test_battery_average_mv_single_sample_is_identity(void) {
    uint32_t sample = 3712;
    TEST_ASSERT_EQUAL_UINT32(3712, battery_average_mv(&sample, 1));
}

void test_battery_average_mv_zero_count_or_null_is_zero(void) {
    uint32_t samples[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT32(0, battery_average_mv(samples, 0));
    TEST_ASSERT_EQUAL_UINT32(0, battery_average_mv(NULL, 4));
}

/* ── battery_charging_from_gpio (wave 2) ─────────────────────────────── */

void test_charging_from_gpio_unwired_is_always_unknown(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, battery_charging_from_gpio(-1, 0, 0));
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, battery_charging_from_gpio(-1, 1, 1));
}

void test_charging_from_gpio_matches_active_level(void) {
    /* Active-low charge-status pin: LOW means charging. */
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES, battery_charging_from_gpio(5, 0, 0));
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_NO, battery_charging_from_gpio(5, 0, 1));

    /* Active-high charge-status pin: HIGH means charging. */
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES, battery_charging_from_gpio(5, 1, 1));
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_NO, battery_charging_from_gpio(5, 1, 0));
}

/* ── battery_beacon_pct (wave 2) ──────────────────────────────────────── */

void test_beacon_pct_emits_sentinel_only_when_confirmed_charging(void) {
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_YES, 100, true));
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_YES, 0, true));
    TEST_ASSERT_EQUAL_UINT8(42, battery_beacon_pct(BATTERY_CHG_NO, 42, true));
    TEST_ASSERT_EQUAL_UINT8(42, battery_beacon_pct(BATTERY_CHG_UNKNOWN, 42, true));
}

/* A board with no battery hardware (present == false, e.g. the nRF null
 * stub) must also get the sentinel: a real pct there is always 0, which on
 * the wire means "dead battery", a false low-battery signal that is worse
 * than the honest "unknown". This must win regardless of the charging
 * value the absent hardware happens to report. */
void test_beacon_pct_emits_sentinel_when_battery_not_present(void) {
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_UNKNOWN, 0, false));
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_NO, 0, false));
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_YES, 0, false));
}

/* ── battery_display_pct_ema (wave 2) ────────────────────────────────── */

void test_display_pct_ema_first_call_snaps_to_raw(void) {
    battery_display_state_t st = {0};
    TEST_ASSERT_EQUAL_UINT8(72, battery_display_pct_ema(&st, 72));
}

/* A NULL state (defensive; no real caller passes one) returns raw_pct
 * unsmoothed instead of dereferencing NULL, consistent with
 * battery_average_mv's NULL guard. */
void test_display_pct_ema_null_state_returns_raw_unsmoothed(void) {
    TEST_ASSERT_EQUAL_UINT8(55, battery_display_pct_ema(NULL, 55));
}

void test_display_pct_ema_settles_gradually_toward_a_step(void) {
    /* Simulates the T-Deck unplug cliff: charge-rail-derived 100% settling
     * toward the true resting-voltage reading of 60%. Each call must move
     * toward 60 without overshooting or jumping there in one step. */
    battery_display_state_t st = {0};
    uint8_t prev = battery_display_pct_ema(&st, 100);
    TEST_ASSERT_EQUAL_UINT8(100, prev);

    bool reached_60 = false;
    for (int i = 0; i < 50 && !reached_60; i++) {
        uint8_t cur = battery_display_pct_ema(&st, 60);
        TEST_ASSERT_TRUE(cur <= prev); /* monotonically settles down */
        TEST_ASSERT_TRUE(cur >= 60);   /* never overshoots past the target */
        prev = cur;
        if (cur == 60)
            reached_60 = true;
    }
    TEST_ASSERT_TRUE(reached_60);
}

void test_display_pct_ema_never_delays_a_drop_into_danger(void) {
    /* Displayed value is comfortably high; the real reading falls straight
     * into (and through) the danger floor in a single call. The smoothed
     * display must reflect that immediately, not gradually. */
    battery_display_state_t st = {0};
    battery_display_pct_ema(&st, 80);
    TEST_ASSERT_EQUAL_UINT8(10, battery_display_pct_ema(&st, 10));
    TEST_ASSERT_EQUAL_UINT8(BATTERY_DANGER_PCT, battery_display_pct_ema(&st, BATTERY_DANGER_PCT));
}

void test_display_pct_ema_independent_states_do_not_interfere(void) {
    battery_display_state_t a = {0};
    battery_display_state_t b = {0};
    battery_display_pct_ema(&a, 90);
    battery_display_pct_ema(&b, 20);
    TEST_ASSERT_EQUAL_UINT8(90, battery_display_pct_ema(&a, 90));
    TEST_ASSERT_EQUAL_UINT8(20, battery_display_pct_ema(&b, 20));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_battery_pct_clamps);
    RUN_TEST(test_battery_pct_breakpoints);
    RUN_TEST(test_battery_pct_interpolates);
    RUN_TEST(test_battery_pct_monotonic);

    RUN_TEST(test_battery_average_mv_simple_mean);
    RUN_TEST(test_battery_average_mv_single_sample_is_identity);
    RUN_TEST(test_battery_average_mv_zero_count_or_null_is_zero);

    RUN_TEST(test_charging_from_gpio_unwired_is_always_unknown);
    RUN_TEST(test_charging_from_gpio_matches_active_level);

    RUN_TEST(test_beacon_pct_emits_sentinel_only_when_confirmed_charging);
    RUN_TEST(test_beacon_pct_emits_sentinel_when_battery_not_present);

    RUN_TEST(test_display_pct_ema_first_call_snaps_to_raw);
    RUN_TEST(test_display_pct_ema_null_state_returns_raw_unsmoothed);
    RUN_TEST(test_display_pct_ema_settles_gradually_toward_a_step);
    RUN_TEST(test_display_pct_ema_never_delays_a_drop_into_danger);
    RUN_TEST(test_display_pct_ema_independent_states_do_not_interfere);
    return UNITY_END();
}
