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

/* ── battery_average_mv ─────────────────────────────────────── */

void test_battery_average_mv_simple_mean(void) {
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT] = {4790, 4800, 4805, 4795, 4800, 4810, 4790, 4800};
    /* Sum = 38390, / 8 = 4798.75 -> 4798 (integer truncation). */
    TEST_ASSERT_EQUAL_UINT32(4798, battery_average_mv(samples, NULL, BATTERY_AVG_SAMPLE_COUNT));
}

void test_battery_average_mv_single_sample_is_identity(void) {
    uint32_t sample = 3712;
    TEST_ASSERT_EQUAL_UINT32(3712, battery_average_mv(&sample, NULL, 1));
}

void test_battery_average_mv_zero_count_or_null_is_zero(void) {
    uint32_t samples[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT32(0, battery_average_mv(samples, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, battery_average_mv(NULL, NULL, 4));
}

/* I3 regression: a single failed conversion must not be folded in as a
 * fabricated 0 mV sample. Without the valid mask, 7 real ~4010 mV samples
 * plus one 0 average to 3508 mV (28070 / 8), a false drop from 83% to 30%
 * that would go out on the wire via the beacon. With the mask, the failed
 * slot is excluded and the average is exactly what the 7 real samples say. */
void test_battery_average_mv_skips_invalid_samples(void) {
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT] = {4010, 4010, 4010, 4010, 4010, 4010, 4010, 0};
    bool valid[BATTERY_AVG_SAMPLE_COUNT] = {true, true, true, true, true, true, true, false};
    uint32_t mv = battery_average_mv(samples, valid, BATTERY_AVG_SAMPLE_COUNT);
    TEST_ASSERT_EQUAL_UINT32(4010, mv);
    TEST_ASSERT_EQUAL_UINT8(83, battery_mv_to_pct(4010));
}

/* Partial failure with a majority still succeeding (6 of 8): the average
 * comes from exactly those 6, not from a division that pretends the other
 * 2 contributed a real value. */
void test_battery_average_mv_six_of_eight_succeed(void) {
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT] = {4000, 4010, 4020, 4030, 0, 4040, 4050, 0};
    bool valid[BATTERY_AVG_SAMPLE_COUNT] = {true, true, true, true, false, true, true, false};
    /* 4000+4010+4020+4030+4040+4050 = 24150, / 6 = 4025 exactly. */
    TEST_ASSERT_EQUAL_UINT32(4025, battery_average_mv(samples, valid, BATTERY_AVG_SAMPLE_COUNT));
}

/* All 8 conversions fail: mv must come out 0, battery_status_t's documented
 * "unavailable" signal, not a number computed from samples that were never
 * actually trustworthy (the array still holds whatever garbage the failed
 * reads left behind). */
void test_battery_average_mv_all_invalid_is_zero_not_corrupted(void) {
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT] = {4010, 4010, 4010, 4010, 4010, 4010, 4010, 4010};
    bool valid[BATTERY_AVG_SAMPLE_COUNT] = {false, false, false, false, false, false, false, false};
    TEST_ASSERT_EQUAL_UINT32(0, battery_average_mv(samples, valid, BATTERY_AVG_SAMPLE_COUNT));
}

/* ── battery_charging_from_gpio ─────────────────────────────── */

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

/* ── battery_infer_charging ─────────────────────────────────── */

/* Bench-measured T-Deck plugged rail range is 4542-4798 mV; 4500 sits
 * comfortably inside it as a representative "obviously plugged" reading. */
void test_infer_charging_unknown_upgrades_to_yes_above_threshold(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES, battery_infer_charging(BATTERY_CHG_UNKNOWN, 4500));
}

/* 4200 mV is a real 1S cell's own physical ceiling: a healthy, fully
 * charged, UNPLUGGED cell can read this high, so it must not infer
 * charging. */
void test_infer_charging_unknown_stays_unknown_at_cell_ceiling(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, battery_infer_charging(BATTERY_CHG_UNKNOWN, 4200));
}

/* 4400 sits in the band the 4350-to-4450 threshold raise exists to
 * protect: high enough that a divider-5 board's amplified ADC error could
 * reach it from a genuinely full unplugged cell, so it must NOT infer
 * charging. Unlike the symbolic boundary test below, this literal fails
 * if the constant is ever lowered back. */
void test_infer_charging_divider5_error_band_stays_unknown(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, battery_infer_charging(BATTERY_CHG_UNKNOWN, 4400));
}

/* Threshold boundary: exactly at BATTERY_MV_CHARGER_RAIL_MIN infers YES
 * (>=), one below does not. */
void test_infer_charging_threshold_boundary(void) {
    TEST_ASSERT_EQUAL_INT(
        BATTERY_CHG_UNKNOWN,
        battery_infer_charging(BATTERY_CHG_UNKNOWN, BATTERY_MV_CHARGER_RAIL_MIN - 1));
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES,
                          battery_infer_charging(BATTERY_CHG_UNKNOWN, BATTERY_MV_CHARGER_RAIL_MIN));
}

/* Hardware truth always wins: a pin-based NO at a voltage that would
 * otherwise infer charging is not second-guessed. A charge-status pin
 * reading NO at 4500 mV is not physically expected, but if it happens,
 * pins still win over an inference the pin itself contradicts. */
void test_infer_charging_never_overrides_pin_no(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_NO, battery_infer_charging(BATTERY_CHG_NO, 4500));
}

/* Hardware truth always wins the other direction too: a pin-based YES at
 * a low voltage (e.g. a charge-complete pin still asserted near a full
 * but unplugged-adjacent cell) is not downgraded. */
void test_infer_charging_never_overrides_pin_yes(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES, battery_infer_charging(BATTERY_CHG_YES, 3700));
}

/* Interaction with an all-failed read (see battery_reading_available): mv
 * comes out 0 in that case, which is nowhere near the inference threshold,
 * so it must stay UNKNOWN rather than the inference logic accidentally
 * treating "no reading" as "definitely not charging" or, worse, tripping
 * on some other zero-adjacent edge case. */
void test_infer_charging_zero_mv_from_failed_read_stays_unknown(void) {
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, battery_infer_charging(BATTERY_CHG_UNKNOWN, 0));
}

/* ── battery_reading_available ───────────────────────────────── */

void test_reading_available_true_when_present_and_nonzero_mv(void) {
    battery_status_t st = {.mv = 4010, .pct = 83, .charging = BATTERY_CHG_NO, .present = true};
    TEST_ASSERT_TRUE(battery_reading_available(&st));
}

/* present stays true through an all-failed read: it reflects ADC init
 * success, not this particular read's outcome. mv/pct both come out 0 in
 * that case (battery_status_t.mv's documented "no reading" sentinel), and
 * that must not count as an available reading despite present == true. */
void test_reading_available_false_when_present_but_all_samples_failed(void) {
    battery_status_t st = {.mv = 0, .pct = 0, .charging = BATTERY_CHG_UNKNOWN, .present = true};
    TEST_ASSERT_FALSE(battery_reading_available(&st));
}

void test_reading_available_false_when_not_present(void) {
    battery_status_t st = {.mv = 4010, .pct = 83, .charging = BATTERY_CHG_NO, .present = false};
    TEST_ASSERT_FALSE(battery_reading_available(&st));
}

void test_reading_available_null_status_is_false(void) {
    TEST_ASSERT_FALSE(battery_reading_available(NULL));
}

/* ── battery_beacon_pct ──────────────────────────────────────── */

void test_beacon_pct_emits_sentinel_only_when_confirmed_charging(void) {
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_YES, 100, true));
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(BATTERY_CHG_YES, 0, true));
    TEST_ASSERT_EQUAL_UINT8(42, battery_beacon_pct(BATTERY_CHG_NO, 42, true));
    TEST_ASSERT_EQUAL_UINT8(42, battery_beacon_pct(BATTERY_CHG_UNKNOWN, 42, true));
}

/* Regression: an all-failed read (present stays true, mv/pct come out 0)
 * must beacon the unknown sentinel, driven through battery_reading_available
 * exactly as mesh_beacon.c does, not a literal 0 that reads as "dead
 * battery" on the wire. */
void test_beacon_pct_emits_sentinel_when_all_samples_failed_despite_present(void) {
    battery_status_t all_failed = {
        .mv = 0, .pct = 0, .charging = BATTERY_CHG_UNKNOWN, .present = true};
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(all_failed.charging, all_failed.pct,
                                                     battery_reading_available(&all_failed)));
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

/* End-to-end regression for a plugged pinless board (the T-Deck): no
 * charge-detect pin (charging starts UNKNOWN), a rail reading well above
 * BATTERY_MV_CHARGER_RAIL_MIN, battery_infer_charging upgrades it to YES
 * exactly as battery_get_status now does, and that YES must still drive
 * battery_beacon_pct to the wire sentinel like a real charge-detect pin
 * would. */
void test_beacon_pct_emits_sentinel_for_voltage_inferred_charging(void) {
    battery_charging_t charging = battery_infer_charging(BATTERY_CHG_UNKNOWN, 4700);
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES, charging);
    TEST_ASSERT_EQUAL_UINT8(0xFF, battery_beacon_pct(charging, battery_mv_to_pct(4700), true));
}

/* ── battery_display_pct_ema ────────────────────────────────── */

void test_display_pct_ema_first_call_snaps_to_raw(void) {
    battery_display_state_t st = {0};
    TEST_ASSERT_EQUAL_UINT8(72, battery_display_pct_ema(&st, 72, 1000));
}

/* A NULL state (defensive; no real caller passes one) returns raw_pct
 * unsmoothed instead of dereferencing NULL, consistent with
 * battery_average_mv's NULL guard. */
void test_display_pct_ema_null_state_returns_raw_unsmoothed(void) {
    TEST_ASSERT_EQUAL_UINT8(55, battery_display_pct_ema(NULL, 55, 1000));
}

void test_display_pct_ema_settles_gradually_toward_a_step(void) {
    /* Simulates the T-Deck unplug cliff: charge-rail-derived 100% settling
     * toward the true resting-voltage reading of 60%. Each call must move
     * toward 60 without overshooting or jumping there in one step. Calls
     * are spaced 500ms apart, comfortably under
     * BATTERY_DISPLAY_SNAP_INTERVAL_MS so smoothing stays active
     * throughout. */
    battery_display_state_t st = {0};
    uint32_t now_ms = 1000;
    uint8_t prev = battery_display_pct_ema(&st, 100, now_ms);
    TEST_ASSERT_EQUAL_UINT8(100, prev);

    bool reached_60 = false;
    for (int i = 0; i < 50 && !reached_60; i++) {
        now_ms += 500;
        uint8_t cur = battery_display_pct_ema(&st, 60, now_ms);
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
    battery_display_pct_ema(&st, 80, 1000);
    TEST_ASSERT_EQUAL_UINT8(10, battery_display_pct_ema(&st, 10, 1500));
    TEST_ASSERT_EQUAL_UINT8(BATTERY_DANGER_PCT,
                            battery_display_pct_ema(&st, BATTERY_DANGER_PCT, 2000));
}

void test_display_pct_ema_independent_states_do_not_interfere(void) {
    battery_display_state_t a = {0};
    battery_display_state_t b = {0};
    battery_display_pct_ema(&a, 90, 1000);
    battery_display_pct_ema(&b, 20, 1000);
    TEST_ASSERT_EQUAL_UINT8(90, battery_display_pct_ema(&a, 90, 1500));
    TEST_ASSERT_EQUAL_UINT8(20, battery_display_pct_ema(&b, 20, 1500));
}

/* ── battery_display_pct_ema: snap on a long gap since the previous call (M2) */

void test_display_pct_ema_snaps_after_a_long_gap(void) {
    /* A caller with a slow render cadence (e.g. an e-paper display at 60s)
     * gets no smoothing benefit: the artifact the EMA masks is a
     * few-seconds-scale rail step, so a gap longer than
     * BATTERY_DISPLAY_SNAP_INTERVAL_MS since the previous call means there
     * is nothing left to mask, only a real reading that should show
     * immediately. */
    battery_display_state_t st = {0};
    battery_display_pct_ema(&st, 100, 0);
    uint32_t long_gap_ms = BATTERY_DISPLAY_SNAP_INTERVAL_MS + 1;
    TEST_ASSERT_EQUAL_UINT8(60, battery_display_pct_ema(&st, 60, long_gap_ms));
}

void test_display_pct_ema_does_not_snap_within_the_interval(void) {
    /* Sanity: a gap exactly at the threshold (not over it) still smooths
     * normally, so the boundary itself does not accidentally snap. */
    battery_display_state_t st = {0};
    uint8_t prev = battery_display_pct_ema(&st, 100, 0);
    uint8_t cur = battery_display_pct_ema(&st, 60, BATTERY_DISPLAY_SNAP_INTERVAL_MS);
    TEST_ASSERT_TRUE(cur < prev);
    TEST_ASSERT_TRUE(cur > 60); /* still mid-settle, not snapped straight to 60 */
}

void test_display_pct_ema_snap_after_gap_still_respects_danger_floor(void) {
    /* Falls out of "snap == return raw_pct" already, pinned down
     * explicitly: a long-gap snap to a genuinely low reading must not
     * somehow re-introduce smoothing on the way down. */
    battery_display_state_t st = {0};
    battery_display_pct_ema(&st, 90, 0);
    uint32_t long_gap_ms = BATTERY_DISPLAY_SNAP_INTERVAL_MS + 1;
    TEST_ASSERT_EQUAL_UINT8(5, battery_display_pct_ema(&st, 5, long_gap_ms));
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
    RUN_TEST(test_battery_average_mv_skips_invalid_samples);
    RUN_TEST(test_battery_average_mv_six_of_eight_succeed);
    RUN_TEST(test_battery_average_mv_all_invalid_is_zero_not_corrupted);

    RUN_TEST(test_charging_from_gpio_unwired_is_always_unknown);
    RUN_TEST(test_charging_from_gpio_matches_active_level);

    RUN_TEST(test_infer_charging_unknown_upgrades_to_yes_above_threshold);
    RUN_TEST(test_infer_charging_unknown_stays_unknown_at_cell_ceiling);
    RUN_TEST(test_infer_charging_divider5_error_band_stays_unknown);
    RUN_TEST(test_infer_charging_threshold_boundary);
    RUN_TEST(test_infer_charging_never_overrides_pin_no);
    RUN_TEST(test_infer_charging_never_overrides_pin_yes);
    RUN_TEST(test_infer_charging_zero_mv_from_failed_read_stays_unknown);

    RUN_TEST(test_reading_available_true_when_present_and_nonzero_mv);
    RUN_TEST(test_reading_available_false_when_present_but_all_samples_failed);
    RUN_TEST(test_reading_available_false_when_not_present);
    RUN_TEST(test_reading_available_null_status_is_false);

    RUN_TEST(test_beacon_pct_emits_sentinel_only_when_confirmed_charging);
    RUN_TEST(test_beacon_pct_emits_sentinel_when_battery_not_present);
    RUN_TEST(test_beacon_pct_emits_sentinel_when_all_samples_failed_despite_present);
    RUN_TEST(test_beacon_pct_emits_sentinel_for_voltage_inferred_charging);

    RUN_TEST(test_display_pct_ema_first_call_snaps_to_raw);
    RUN_TEST(test_display_pct_ema_null_state_returns_raw_unsmoothed);
    RUN_TEST(test_display_pct_ema_settles_gradually_toward_a_step);
    RUN_TEST(test_display_pct_ema_never_delays_a_drop_into_danger);
    RUN_TEST(test_display_pct_ema_independent_states_do_not_interfere);
    RUN_TEST(test_display_pct_ema_snaps_after_a_long_gap);
    RUN_TEST(test_display_pct_ema_does_not_snap_within_the_interval);
    RUN_TEST(test_display_pct_ema_snap_after_gap_still_respects_danger_floor);
    return UNITY_END();
}
