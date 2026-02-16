#include "unity.h"
#include "../components/timesync/timesync.c"

void setUp(void) {}
void tearDown(void) {}

void test_init_unsynchronized(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    TEST_ASSERT_EQUAL_UINT8(MAX_STRATUM, ts.stratum);
    TEST_ASSERT_FALSE(ts.synchronized);
    TEST_ASSERT_EQUAL_INT64(0, ts.offset_ms);
}

void test_accept_better_stratum(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    // Unsync'd (stratum 7) should accept stratum 1
    int ret = timesync_handle_sync(&ts, 100000, 1, 50000);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_TRUE(ts.synchronized);
}

void test_reject_worse_stratum(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    // First sync to stratum 2
    timesync_handle_sync(&ts, 100000, 1, 50000);
    TEST_ASSERT_EQUAL_UINT8(2, ts.stratum);
    // Reject stratum 3 (worse)
    int ret = timesync_handle_sync(&ts, 100100, 3, 50100);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_max_shift_enforced(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    // First sync: offset = 100000 - 50000 = 50000
    timesync_handle_sync(&ts, 100000, 1, 50000);
    // Second sync with huge shift: offset would be 200000 - 50100 = 149900, shift = 99900 > 5000
    int ret = timesync_handle_sync(&ts, 200000, 0, 50100);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

void test_stratum_propagation(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    timesync_handle_sync(&ts, 100000, 1, 50000);
    TEST_ASSERT_EQUAL_UINT8(2, timesync_get_stratum(&ts));
}

void test_should_emit_only_low_stratum(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    // Sync to stratum 2
    timesync_handle_sync(&ts, 100000, 1, 50000);
    TEST_ASSERT_EQUAL_UINT8(2, ts.stratum);
    // Should emit (stratum 2, enough time elapsed since last_emit=0)
    TEST_ASSERT_TRUE(timesync_should_emit(&ts, 400000));

    // Now make a node at stratum 5
    timesync_state_t ts2;
    timesync_init(&ts2);
    timesync_handle_sync(&ts2, 100000, 4, 50000);
    TEST_ASSERT_EQUAL_UINT8(5, ts2.stratum);
    TEST_ASSERT_FALSE(timesync_should_emit(&ts2, 400000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_unsynchronized);
    RUN_TEST(test_accept_better_stratum);
    RUN_TEST(test_reject_worse_stratum);
    RUN_TEST(test_max_shift_enforced);
    RUN_TEST(test_stratum_propagation);
    RUN_TEST(test_should_emit_only_low_stratum);
    return UNITY_END();
}
