#include "unity.h"
#include "timesync.h"

void setUp(void) {}
void tearDown(void) {}

void test_weighted_average_prefers_better_stratum(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    TEST_ASSERT_EQUAL_INT(0, timesync_handle_sync(&ts, 1450, 6, 1000)); /* +450, low weight */
    TEST_ASSERT_EQUAL_INT(0, timesync_handle_sync(&ts, 1100, 1, 1000)); /* +100, high weight */

    /* Weighted avg: (450*1 + 100*6) / 7 = 150 */
    TEST_ASSERT_EQUAL_INT64(150, ts.offset_ms);
}

void test_stratum_selection_rejects_non_improving_sources(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    TEST_ASSERT_EQUAL_INT(0, timesync_handle_sync(&ts, 1200, 1, 1000));
    TEST_ASSERT_EQUAL_UINT8(2, timesync_get_stratum(&ts));

    /* Remote stratum 2 is not better than current local stratum 2 */
    TEST_ASSERT_EQUAL_INT(-1, timesync_handle_sync(&ts, 1300, 2, 1000));
}

void test_rejection_threshold_for_large_shift_when_synced(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    TEST_ASSERT_EQUAL_INT(0, timesync_handle_sync(&ts, 3000, 6, 1000)); /* offset 2000 */
    TEST_ASSERT_EQUAL_INT64(2000, ts.offset_ms);

    /* Proposed offset 5001 => shift 3001 (> MAX_TIME_SHIFT_MS=2000) */
    TEST_ASSERT_EQUAL_INT(-2, timesync_handle_sync(&ts, 6001, 0, 1000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_weighted_average_prefers_better_stratum);
    RUN_TEST(test_stratum_selection_rejects_non_improving_sources);
    RUN_TEST(test_rejection_threshold_for_large_shift_when_synced);
    return UNITY_END();
}
