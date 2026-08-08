/*
 * Host tests for the radio reinit retry policy.
 *
 * The invariant this exists to hold: a radio recovery that fails is owed
 * another attempt. Both backends used to clear the reinit latch before
 * attempting recovery and then drop the request if the attempt failed, which
 * turned one bad recovery into a permanently silent radio on a node that still
 * looked healthy from every other angle. The policy also has to keep that
 * retry off the mesh loop's 10ms cadence, because a full recovery hard-resets
 * and recalibrates the chip.
 */

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* A fresh policy has no debt: the first request is acted on immediately. */
static void test_fresh_policy_attempts_immediately(void) {
    radio_reinit_policy_t p = {0};
    TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, 0u));
    TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, 1234567u));
}

/* A successful recovery leaves nothing owed, so a later independent request is
 * acted on at once rather than waiting out a backoff it did not earn. */
static void test_success_leaves_no_backoff(void) {
    radio_reinit_policy_t p = {0};
    radio_reinit_policy_on_result(&p, true, 10000u);
    TEST_ASSERT_FALSE(p.retry_pending);
    TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, 10001u));
}

/* The core case: a failure schedules a retry, and that retry is genuinely
 * attempted once the backoff expires. */
static void test_failure_retries_after_backoff(void) {
    radio_reinit_policy_t p = {0};
    radio_reinit_policy_on_result(&p, false, 10000u);
    TEST_ASSERT_TRUE(p.retry_pending);

    TEST_ASSERT_FALSE(radio_reinit_policy_should_attempt(&p, 10000u));
    TEST_ASSERT_FALSE(radio_reinit_policy_should_attempt(&p, 10001u));
    TEST_ASSERT_FALSE(
        radio_reinit_policy_should_attempt(&p, 10000u + BRAMBLE_RADIO_REINIT_RETRY_MS - 1u));
    TEST_ASSERT_TRUE(
        radio_reinit_policy_should_attempt(&p, 10000u + BRAMBLE_RADIO_REINIT_RETRY_MS));
    TEST_ASSERT_TRUE(
        radio_reinit_policy_should_attempt(&p, 10000u + BRAMBLE_RADIO_REINIT_RETRY_MS + 60000u));
}

/* Repeated failures keep retrying forever rather than giving up, each one a
 * full backoff after the last. A chip that is genuinely dead costs one
 * bring-up attempt every BRAMBLE_RADIO_REINIT_RETRY_MS and no more. */
static void test_repeated_failure_keeps_retrying(void) {
    radio_reinit_policy_t p = {0};
    uint32_t t = 10000u;
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, t));
        radio_reinit_policy_on_result(&p, false, t);
        TEST_ASSERT_FALSE(radio_reinit_policy_should_attempt(&p, t + 1u));
        t += BRAMBLE_RADIO_REINIT_RETRY_MS;
    }
    /* And it still recovers cleanly when an attempt finally works. */
    radio_reinit_policy_on_result(&p, true, t);
    TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, t));
}

/* The deadline is compared as a wrap-safe unsigned difference, so a failure
 * recorded just before the millisecond clock's 49.7-day rollover still retries
 * on time instead of stalling for another 49.7 days. */
static void test_backoff_survives_clock_wrap(void) {
    radio_reinit_policy_t p = {0};
    uint32_t before_wrap = 0xFFFFFFFFu - 1000u;
    radio_reinit_policy_on_result(&p, false, before_wrap);
    /* Deadline itself has wrapped past zero. */
    TEST_ASSERT_TRUE(p.retry_after_ms < before_wrap);

    TEST_ASSERT_FALSE(radio_reinit_policy_should_attempt(&p, before_wrap + 1u));
    TEST_ASSERT_FALSE(radio_reinit_policy_should_attempt(&p, 0xFFFFFFFFu));
    TEST_ASSERT_FALSE(radio_reinit_policy_should_attempt(&p, 100u));
    TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, p.retry_after_ms));
    TEST_ASSERT_TRUE(radio_reinit_policy_should_attempt(&p, p.retry_after_ms + 1u));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_policy_attempts_immediately);
    RUN_TEST(test_success_leaves_no_backoff);
    RUN_TEST(test_failure_retries_after_backoff);
    RUN_TEST(test_repeated_failure_keeps_retrying);
    RUN_TEST(test_backoff_survives_clock_wrap);
    return UNITY_END();
}
