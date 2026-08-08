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

void test_stale_threshold(void) {
    TEST_ASSERT_FALSE(node_is_stale(0));
    TEST_ASSERT_FALSE(node_is_stale(NODE_STALE_AGE_S - 1));
    TEST_ASSERT_TRUE(node_is_stale(NODE_STALE_AGE_S));
    TEST_ASSERT_TRUE(node_is_stale(NODE_STALE_AGE_S * 10));
}

void test_signal_pct_maps_and_clamps(void) {
    /* -120 dBm is the floor of the usable window, -50 the ceiling. */
    TEST_ASSERT_EQUAL_INT(0, node_signal_pct(-120));
    TEST_ASSERT_EQUAL_INT(0, node_signal_pct(-128));
    TEST_ASSERT_EQUAL_INT(100, node_signal_pct(-50));
    TEST_ASSERT_EQUAL_INT(100, node_signal_pct(0));
    TEST_ASSERT_EQUAL_INT(50, node_signal_pct(-85));
}

void test_reach_online_only_while_a_neighbor_is_fresh(void) {
    TEST_ASSERT_EQUAL(NODE_REACH_ONLINE, node_reach_classify(true, 0, false));
    TEST_ASSERT_EQUAL(NODE_REACH_ONLINE, node_reach_classify(true, NODE_ONLINE_AGE_S - 1, false));
    TEST_ASSERT_EQUAL(NODE_REACH_REACHABLE, node_reach_classify(true, NODE_ONLINE_AGE_S, false));
}

void test_reach_online_window_is_tighter_than_the_row_dim_threshold(void) {
    /* The two thresholds answer different questions and must not be collapsed:
     * a peer can read "not online" in a chat header while its Nodes row is
     * still styled as live. */
    TEST_ASSERT_TRUE(NODE_ONLINE_AGE_S < NODE_STALE_AGE_S);
    TEST_ASSERT_EQUAL(NODE_REACH_REACHABLE, node_reach_classify(true, NODE_ONLINE_AGE_S, false));
    TEST_ASSERT_FALSE(node_is_stale(NODE_ONLINE_AGE_S));
}

void test_reach_quiet_neighbor_stays_reachable_not_unknown(void) {
    /* It has not been purged, so the last thing we know is that it was there. */
    TEST_ASSERT_EQUAL(NODE_REACH_REACHABLE,
                      node_reach_classify(true, NODE_STALE_AGE_S * 100, false));
}

void test_reach_route_only_peer_is_reachable(void) {
    TEST_ASSERT_EQUAL(NODE_REACH_REACHABLE, node_reach_classify(false, 0, true));
}

void test_reach_unknown_with_neither_neighbor_nor_route(void) {
    TEST_ASSERT_EQUAL(NODE_REACH_UNKNOWN, node_reach_classify(false, 0, false));
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
    RUN_TEST(test_stale_threshold);
    RUN_TEST(test_signal_pct_maps_and_clamps);
    RUN_TEST(test_reach_online_only_while_a_neighbor_is_fresh);
    RUN_TEST(test_reach_online_window_is_tighter_than_the_row_dim_threshold);
    RUN_TEST(test_reach_quiet_neighbor_stays_reachable_not_unknown);
    RUN_TEST(test_reach_route_only_peer_is_reachable);
    RUN_TEST(test_reach_unknown_with_neither_neighbor_nor_route);
    RUN_TEST(test_format_age_seconds);
    RUN_TEST(test_format_age_minutes_keep_the_seconds_digit);
    RUN_TEST(test_format_age_hours_and_days);
    RUN_TEST(test_format_age_rejects_empty_buffer);
    return UNITY_END();
}
