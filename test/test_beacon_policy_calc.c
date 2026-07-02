#include "unity.h"
#include "beacon_policy_calc.h"

void setUp(void) {}
void tearDown(void) {}

void test_empty_history_zero_events(void) {
    churn_sample_t h[MAX_CHURN_HISTORY] = {0};
    TEST_ASSERT_EQUAL_UINT8(0, beacon_churn_count(h, MAX_CHURN_HISTORY, 1000u, 60000u));
}

void test_stable_count_zero_events(void) {
    churn_sample_t h[MAX_CHURN_HISTORY] = {0};
    h[0] = (churn_sample_t){.timestamp = 100u, .neighbor_count = 3};
    h[1] = (churn_sample_t){.timestamp = 200u, .neighbor_count = 3};
    h[2] = (churn_sample_t){.timestamp = 300u, .neighbor_count = 3};
    TEST_ASSERT_EQUAL_UINT8(0, beacon_churn_count(h, MAX_CHURN_HISTORY, 1000u, 60000u));
}

void test_changes_counted_as_events(void) {
    churn_sample_t h[MAX_CHURN_HISTORY] = {0};
    h[0] = (churn_sample_t){.timestamp = 100u, .neighbor_count = 3};
    h[1] = (churn_sample_t){.timestamp = 200u, .neighbor_count = 4};
    h[2] = (churn_sample_t){.timestamp = 300u, .neighbor_count = 4};
    h[3] = (churn_sample_t){.timestamp = 400u, .neighbor_count = 5};
    TEST_ASSERT_EQUAL_UINT8(2, beacon_churn_count(h, MAX_CHURN_HISTORY, 1000u, 60000u));
}

void test_out_of_window_ignored(void) {
    churn_sample_t h[MAX_CHURN_HISTORY] = {0};
    /* now=100000, window=60000 -> anything with timestamp < 40000 dropped. */
    h[0] = (churn_sample_t){.timestamp = 10000u, .neighbor_count = 3};  /* dropped */
    h[1] = (churn_sample_t){.timestamp = 50000u, .neighbor_count = 4};  /* first kept */
    h[2] = (churn_sample_t){.timestamp = 60000u, .neighbor_count = 5};  /* +1 event */
    TEST_ASSERT_EQUAL_UINT8(1, beacon_churn_count(h, MAX_CHURN_HISTORY, 100000u, 60000u));
}

void test_decide_fixed_when_disabled(void) {
    beacon_interval_decision_t d =
        beacon_interval_decide(0 /*enabled*/, 1 /*adaptive*/,
                               60000u, 30000u, 120000u, 10, 3, 20, 9);
    TEST_ASSERT_EQUAL_UINT32(60000u, d.interval_ms);
    TEST_ASSERT_EQUAL_INT(0, d.adaptive_active);
    TEST_ASSERT_EQUAL_INT(0, d.in_backoff);
}

void test_decide_fixed_when_mode_not_adaptive(void) {
    beacon_interval_decision_t d =
        beacon_interval_decide(1, 0 /*not adaptive*/,
                               60000u, 30000u, 120000u, 10, 3, 20, 9);
    TEST_ASSERT_EQUAL_UINT32(60000u, d.interval_ms);
    TEST_ASSERT_EQUAL_INT(0, d.adaptive_active);
}

void test_decide_dense_backs_off_to_max(void) {
    beacon_interval_decision_t d =
        beacon_interval_decide(1, 1, 60000u, 30000u, 120000u, 10, 3,
                               10 /*neighbors == dense_threshold*/, 0);
    TEST_ASSERT_EQUAL_UINT32(120000u, d.interval_ms);
    TEST_ASSERT_EQUAL_INT(1, d.in_backoff);
    TEST_ASSERT_EQUAL_INT(1, d.adaptive_active);
}

void test_decide_churn_speeds_up_to_min(void) {
    beacon_interval_decision_t d =
        beacon_interval_decide(1, 1, 60000u, 30000u, 120000u, 10, 3,
                               4 /*below dense*/, 3 /*churn == threshold*/);
    TEST_ASSERT_EQUAL_UINT32(30000u, d.interval_ms);
    TEST_ASSERT_EQUAL_INT(0, d.in_backoff);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_history_zero_events);
    RUN_TEST(test_stable_count_zero_events);
    RUN_TEST(test_changes_counted_as_events);
    RUN_TEST(test_out_of_window_ignored);
    RUN_TEST(test_decide_fixed_when_disabled);
    RUN_TEST(test_decide_fixed_when_mode_not_adaptive);
    RUN_TEST(test_decide_dense_backs_off_to_max);
    RUN_TEST(test_decide_churn_speeds_up_to_min);
    return UNITY_END();
}
