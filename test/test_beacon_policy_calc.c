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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_history_zero_events);
    RUN_TEST(test_stable_count_zero_events);
    RUN_TEST(test_changes_counted_as_events);
    RUN_TEST(test_out_of_window_ignored);
    return UNITY_END();
}
