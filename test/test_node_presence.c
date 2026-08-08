#include "unity.h"
#include "node_presence.h"

void setUp(void) {}
void tearDown(void) {}

void test_age_seconds_truncates_to_whole_seconds(void) {
    TEST_ASSERT_EQUAL_UINT32(0, node_age_seconds(1999, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, node_age_seconds(2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(90, node_age_seconds(91000, 1000));
}

void test_age_seconds_clamps_last_heard_in_the_future(void) {
    /* The caller samples now_ms and the neighbor snapshot separately, so a
     * beacon landing between the two reads yields last_heard > now_ms. That
     * must read as "just now", not as an unsigned-underflow 49-day age. */
    TEST_ASSERT_EQUAL_UINT32(0, node_age_seconds(1000, 2000));
    TEST_ASSERT_EQUAL_UINT32(0, node_age_seconds(0, 0xFFFFFFFFu));
}

void test_presence_live_below_stale_threshold(void) {
    TEST_ASSERT_EQUAL(NODE_PRESENCE_LIVE, node_presence_for_age(0));
    TEST_ASSERT_EQUAL(NODE_PRESENCE_LIVE, node_presence_for_age(NODE_STALE_AGE_S - 1));
}

void test_presence_stale_at_and_above_threshold(void) {
    TEST_ASSERT_EQUAL(NODE_PRESENCE_STALE, node_presence_for_age(NODE_STALE_AGE_S));
    TEST_ASSERT_EQUAL(NODE_PRESENCE_STALE, node_presence_for_age(NODE_STALE_AGE_S * 10));
}

void test_format_age_seconds(void) {
    char buf[16];
    node_format_age(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0s", buf);
    node_format_age(59, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("59s", buf);
}

void test_format_age_minutes_keep_the_seconds_digit(void) {
    /* The whole point: at the ages a neighbor table actually holds (under the
     * 10 min expiry) the string must change every second, not once a minute. */
    char buf[16];
    node_format_age(60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1m 0s", buf);
    node_format_age(252, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("4m 12s", buf);
    node_format_age(253, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("4m 13s", buf);
    node_format_age(3599, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("59m 59s", buf);
}

void test_format_age_hours_and_days(void) {
    char buf[16];
    node_format_age(3600, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1h 0m", buf);
    node_format_age(8040, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2h 14m", buf);
    node_format_age(86400, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1d 0h", buf);
    node_format_age(273600, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3d 4h", buf);
}

void test_format_age_rejects_empty_buffer(void) {
    char buf[1] = {0x7F};
    TEST_ASSERT_EQUAL(0, node_format_age(42, NULL, 8));
    TEST_ASSERT_EQUAL(0, node_format_age(42, buf, 0));
    TEST_ASSERT_EQUAL(0x7F, buf[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_age_seconds_truncates_to_whole_seconds);
    RUN_TEST(test_age_seconds_clamps_last_heard_in_the_future);
    RUN_TEST(test_presence_live_below_stale_threshold);
    RUN_TEST(test_presence_stale_at_and_above_threshold);
    RUN_TEST(test_format_age_seconds);
    RUN_TEST(test_format_age_minutes_keep_the_seconds_digit);
    RUN_TEST(test_format_age_hours_and_days);
    RUN_TEST(test_format_age_rejects_empty_buffer);
    return UNITY_END();
}
