#include "unity.h"
#include "replay_window.h"
#include "../components/replay_window/replay_window.c"
#include "replay_deferred.h"
#include "../components/replay_window/replay_deferred.c"

static replay_table_t t;
static replay_deferred_t d;
void setUp(void) { replay_table_init(&t); replay_deferred_init(&d); }
void tearDown(void) {}

void test_first_counter_accepted(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 100, 0));
}
void test_exact_replay_rejected(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 100, 0));
}
void test_forward_shift_accepts(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 200, 0));
}
void test_in_window_reorder_accepts_once(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 90, 0));      /* within 64 */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 90, 0));  /* dup */
}
void test_below_window_flagged(void) {
    replay_check_and_add(&t, 0xAA, 1000, 0);
    TEST_ASSERT_EQUAL(REPLAY_BELOW_WINDOW, replay_check_and_add(&t, 0xAA, 100, 0));
}
void test_reboot_higher_counter_accepted(void) {
    /* sender reboots and resumes above its old ceiling: a big jump forward is fine */
    replay_check_and_add(&t, 0xAA, 500, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 70000, 0));
}
void test_distinct_senders_independent(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xBB, 100, 0));
}

/* BUG A regression: a fresh slot must be distinguished from "high_water is
 * legitimately 0", or a first-ever counter of 0 (the nonce counter's actual
 * first-boot value, see Task 0.4) is replayable forever since every replay
 * of counter 0 re-hits the "fresh slot" sentinel instead of the dup check. */
void test_first_counter_zero_then_replay_rejected(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 0, 0));
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 0, 0));
}

/* BUG B regression: an exact 64-counter forward jump must still remember the
 * old high_water at window bit 63; a naive `shift >= 64 -> window = 0` wipes
 * it and lets the old high_water be replayed. */
void test_exact_64_jump_then_replay_old_high_water_rejected(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 164, 0)); /* +64 exactly */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 100, 0)); /* replay old */
}

/* Deferred (tier-2) acceptance: Task 0.6. */
void test_deferred_accepts_fresh_then_dedups(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT,
        replay_deferred_accept(&d, 0xAA, 5, 1000, 2000, 1));
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP,
        replay_deferred_accept(&d, 0xAA, 5, 1000, 2000, 1));
}
void test_deferred_rejects_expired(void) {
    TEST_ASSERT_NOT_EQUAL(REPLAY_ACCEPT,
        replay_deferred_accept(&d, 0xAA, 5, 1000, 1000 + 90000, 1)); /* > 24h old */
}
void test_deferred_fail_closed_when_timesync_untrusted(void) {
    TEST_ASSERT_NOT_EQUAL(REPLAY_ACCEPT,
        replay_deferred_accept(&d, 0xAA, 5, 1000, 2000, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_counter_accepted);
    RUN_TEST(test_exact_replay_rejected);
    RUN_TEST(test_forward_shift_accepts);
    RUN_TEST(test_in_window_reorder_accepts_once);
    RUN_TEST(test_below_window_flagged);
    RUN_TEST(test_reboot_higher_counter_accepted);
    RUN_TEST(test_distinct_senders_independent);
    RUN_TEST(test_first_counter_zero_then_replay_rejected);
    RUN_TEST(test_exact_64_jump_then_replay_old_high_water_rejected);
    RUN_TEST(test_deferred_accepts_fresh_then_dedups);
    RUN_TEST(test_deferred_rejects_expired);
    RUN_TEST(test_deferred_fail_closed_when_timesync_untrusted);
    return UNITY_END();
}
