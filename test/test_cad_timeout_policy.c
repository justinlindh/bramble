/*
 * Host coverage for the CAD-timeout fail-open/closed policy (issue #118).
 *
 * The decision itself is a pure helper in radio.h shared by radio_esp.c and
 * radio_virt.c: fail open (transmit anyway) for the first
 * BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD-1 consecutive CAD timeouts, then fail
 * closed (report busy, request reinit) on the threshold-th, resetting the run
 * on the trip and on any completed CAD. This suite exercises that logic
 * directly; the driver wiring is covered by test_radio_virt.
 */

#include "unity.h"
#include "radio.h"

void setUp(void) {}
void tearDown(void) {}

/* A single timeout must never trip: the middle path exists precisely so a
 * transient missed IRQ does not fail closed or reinit the radio. */
void test_a_single_timeout_fails_open(void) {
    TEST_ASSERT_TRUE(BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD >= 2u);
    cad_timeout_policy_t p = {0};
    TEST_ASSERT_EQUAL_INT(CAD_TIMEOUT_FAIL_OPEN, cad_timeout_policy_on_timeout(&p));
}

/* Fail open up to the threshold, then fail closed exactly on it. */
void test_fails_open_until_threshold_then_closed(void) {
    cad_timeout_policy_t p = {0};
    for (unsigned i = 1; i < BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD; i++) {
        TEST_ASSERT_EQUAL_INT(CAD_TIMEOUT_FAIL_OPEN, cad_timeout_policy_on_timeout(&p));
    }
    TEST_ASSERT_EQUAL_INT(CAD_TIMEOUT_FAIL_CLOSED, cad_timeout_policy_on_timeout(&p));
}

/* A completed CAD clears the streak, so a later lone timeout fails open again. */
void test_success_resets_the_run(void) {
    cad_timeout_policy_t p = {0};
    for (unsigned i = 1; i < BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD; i++) {
        cad_timeout_policy_on_timeout(&p);
    }
    cad_timeout_policy_on_success(&p);
    TEST_ASSERT_EQUAL_INT(CAD_TIMEOUT_FAIL_OPEN, cad_timeout_policy_on_timeout(&p));
}

/* The trip resets the counter so it does not fail closed on every subsequent
 * call: the next timeout after a trip fails open again, giving the reinit it
 * requested a chance to recover the radio first. */
void test_trip_rearms_for_the_next_run(void) {
    cad_timeout_policy_t p = {0};
    for (unsigned i = 1; i < BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD; i++) {
        cad_timeout_policy_on_timeout(&p);
    }
    TEST_ASSERT_EQUAL_INT(CAD_TIMEOUT_FAIL_CLOSED, cad_timeout_policy_on_timeout(&p));
    TEST_ASSERT_EQUAL_INT(CAD_TIMEOUT_FAIL_OPEN, cad_timeout_policy_on_timeout(&p));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_single_timeout_fails_open);
    RUN_TEST(test_fails_open_until_threshold_then_closed);
    RUN_TEST(test_success_resets_the_run);
    RUN_TEST(test_trip_rearms_for_the_next_run);
    return UNITY_END();
}
