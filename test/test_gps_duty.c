/*
 * GPS duty-cycling policy (Task 9): pure decision of whether GNSS power
 * should be on right now, given the persisted user preference and the
 * location-share policy state. One test per rule in gps_duty.h, plus a
 * uint32 wraparound case for the mesh clock (which wraps every ~49.7 days).
 */
#include "unity.h"
#include "gps_duty.h"

#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

/* Rule 1: !user_enabled -> false, always, regardless of sharing state. */
void test_user_disabled_is_always_off(void) {
    gps_duty_inputs_t in = {
        .user_enabled = false,
        .sharing_active = true,
        .interval_s = 300,
        .now_ms = 1000000,
        .last_send_ms = 0,
    };
    TEST_ASSERT_FALSE(gps_duty_should_power(&in));

    in.sharing_active = false;
    TEST_ASSERT_FALSE(gps_duty_should_power(&in));
}

/* Rule 2: enabled but not sharing -> GPS follows the preference (RPC/map
 * still want live fixes even when nothing is being shared to the mesh). */
void test_enabled_not_sharing_is_always_on(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = false,
        .interval_s = 300,
        .now_ms = 1000000,
        .last_send_ms = 999000,
    };
    TEST_ASSERT_TRUE(gps_duty_should_power(&in));
}

/* Rule 3: sharing with an interval below the min-worth-cycling floor stays
 * on, no matter how long ago the clock or last send suggest. */
void test_short_interval_below_min_stays_on(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = true,
        .interval_s = GPS_DUTY_MIN_INTERVAL_S - 1,
        .now_ms = 1000000,
        .last_send_ms = 1,
    };
    TEST_ASSERT_TRUE(gps_duty_should_power(&in));
}

/* Rule 4: never sent yet -> on, so the first share has a fix to send. */
void test_never_sent_forces_on_for_first_fix(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = true,
        .interval_s = 300,
        .now_ms = 1000,
        .last_send_ms = 0,
    };
    TEST_ASSERT_TRUE(gps_duty_should_power(&in));
}

/* Rule 5: right at the warm-margin boundary before the next scheduled
 * send, power comes on (<=, not <). last_send_ms is deliberately nonzero
 * here (and below) so rule 4 ("never sent") does not shadow rule 5: with
 * last_send_ms=1000 and interval 300s, the next send is due at 301000ms,
 * so 241000ms (301000 - 60000 margin) is exactly on time. */
void test_wakes_at_warm_margin_boundary(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = true,
        .interval_s = 300,
        .last_send_ms = 1000,
        .now_ms = 1000 + 300000u - GPS_DUTY_WARM_MARGIN_S * 1000u,
    };
    TEST_ASSERT_TRUE(gps_duty_should_power(&in));
}

/* Rule 5: one millisecond before that boundary, power stays off. */
void test_sleeps_just_outside_warm_margin(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = true,
        .interval_s = 300,
        .last_send_ms = 1000,
        .now_ms = 1000 + 300000u - GPS_DUTY_WARM_MARGIN_S * 1000u - 1u,
    };
    TEST_ASSERT_FALSE(gps_duty_should_power(&in));
}

/* Rule 5: an overdue send (past the scheduled time) is also "on". */
void test_overdue_send_is_on(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = true,
        .interval_s = 300,
        .last_send_ms = 1000,
        .now_ms = 1000 + 301000u, /* 1s past the scheduled send */
    };
    TEST_ASSERT_TRUE(gps_duty_should_power(&in));
}

/* Wraparound: the mesh clock is a wrapping uint32_t millisecond counter.
 * last_send_ms sits just before the wrap; now_ms has wrapped back around to
 * a small value. Chronologically now is 105s after last_send_ms (5s to
 * reach the wrap, plus 100s past it), so the signed wrap-safe subtraction
 * inside gps_duty_should_power must still resolve correctly on both sides
 * of the warm margin. */
void test_wraparound_math_is_signed_safe(void) {
    gps_duty_inputs_t in = {
        .user_enabled = true,
        .sharing_active = true,
        .interval_s = 130, /* > GPS_DUTY_MIN_INTERVAL_S */
        .last_send_ms = UINT32_MAX - 5000u,
        .now_ms = 100000u, /* 105s after last_send_ms, post-wrap */
    };
    /* Next send is due 130s after last_send_ms; now is 105s after, so due
     * in 25s, inside the 60s warm margin: power on. */
    TEST_ASSERT_TRUE(gps_duty_should_power(&in));

    in.now_ms = 50000u; /* only 55s after last_send_ms; due in 75s: off. */
    TEST_ASSERT_FALSE(gps_duty_should_power(&in));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_user_disabled_is_always_off);
    RUN_TEST(test_enabled_not_sharing_is_always_on);
    RUN_TEST(test_short_interval_below_min_stays_on);
    RUN_TEST(test_never_sent_forces_on_for_first_fix);
    RUN_TEST(test_wakes_at_warm_margin_boundary);
    RUN_TEST(test_sleeps_just_outside_warm_margin);
    RUN_TEST(test_overdue_send_is_on);
    RUN_TEST(test_wraparound_math_is_signed_safe);
    return UNITY_END();
}
