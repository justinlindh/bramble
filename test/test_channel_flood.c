#include "unity.h"
#include "../components/routing/channel_flood.c"
#include "../components/routing/discovery.c"
#include "../components/routing/routing.c"

void setUp(void) {}
void tearDown(void) {}

/* --- Hop-limit floor: a relay receiving hop_limit <= 1 never forwards,
 * matching forward_data()/RREQ's ">1" convention (hop_limit N means
 * exactly N-hop reach). --- */

void test_hop_limit_exhausted_at_one_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(1, false, true, 42);
    TEST_ASSERT_FALSE(d.should_relay);
    TEST_ASSERT_EQUAL(0, d.new_hop_limit);
    TEST_ASSERT_EQUAL(0, d.jitter_ms);
}

void test_hop_limit_zero_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(0, false, true, 42);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_hop_limit_two_relays_and_decrements_to_one(void) {
    channel_flood_decision_t d = channel_flood_decide(2, false, true, 42);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(1, d.new_hop_limit);
}

/* --- Duplicate suppression: a broadcast already seen (per the caller's
 * dedup lookup) is never relayed, regardless of hop budget or airtime. --- */

void test_duplicate_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(8, true, true, 42);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_duplicate_with_ample_hop_budget_and_free_airtime_still_drops(void) {
    channel_flood_decision_t d = channel_flood_decide(255, true, true, 0);
    TEST_ASSERT_FALSE(d.should_relay);
}

/* --- Airtime-aware relay: a budget-denied node stops relaying rather than
 * amplifying a storm. This is the scale-sensitive lever. --- */

void test_budget_denied_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(8, false, false, 42);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_budget_denied_beats_otherwise_healthy_decision(void) {
    /* Plenty of hop budget, not a duplicate -- only the airtime budget
     * says no, and that alone must be enough to stop the relay. */
    channel_flood_decision_t d = channel_flood_decide(8, false, false, 100);
    TEST_ASSERT_FALSE(d.should_relay);
    TEST_ASSERT_EQUAL(0, d.new_hop_limit);
    TEST_ASSERT_EQUAL(0, d.jitter_ms);
}

/* --- Healthy relay path: decrements hop_limit by exactly one and draws
 * jitter from the shared RREQ_FWD_JITTER range (DES-3), not a second
 * hardcoded constant set. --- */

void test_healthy_relay_decrements_hop_limit_by_one(void) {
    channel_flood_decision_t d = channel_flood_decide(8, false, true, 10);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(7, d.new_hop_limit);
}

void test_healthy_relay_max_hop_limit(void) {
    channel_flood_decision_t d = channel_flood_decide(255, false, true, 10);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(254, d.new_hop_limit);
}

void test_jitter_reuses_rreq_forward_range(void) {
    /* channel_flood_decide's jitter must be discovery_forward_jitter_ms
     * verbatim (same range, same mapping), not an independent constant
     * set: exercise the same boundary values test_discovery.c uses. */
    channel_flood_decision_t d0 = channel_flood_decide(8, false, true, 0);
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MIN_MS, d0.jitter_ms);

    uint32_t span = RREQ_FWD_JITTER_MAX_MS - RREQ_FWD_JITTER_MIN_MS;
    channel_flood_decision_t dmax = channel_flood_decide(8, false, true, span);
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MAX_MS, dmax.jitter_ms);

    for (uint32_t r = 0; r < 2000; r += 17) {
        channel_flood_decision_t d = channel_flood_decide(8, false, true, r);
        TEST_ASSERT_TRUE(d.jitter_ms >= RREQ_FWD_JITTER_MIN_MS &&
                         d.jitter_ms <= RREQ_FWD_JITTER_MAX_MS);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hop_limit_exhausted_at_one_does_not_relay);
    RUN_TEST(test_hop_limit_zero_does_not_relay);
    RUN_TEST(test_hop_limit_two_relays_and_decrements_to_one);
    RUN_TEST(test_duplicate_does_not_relay);
    RUN_TEST(test_duplicate_with_ample_hop_budget_and_free_airtime_still_drops);
    RUN_TEST(test_budget_denied_does_not_relay);
    RUN_TEST(test_budget_denied_beats_otherwise_healthy_decision);
    RUN_TEST(test_healthy_relay_decrements_hop_limit_by_one);
    RUN_TEST(test_healthy_relay_max_hop_limit);
    RUN_TEST(test_jitter_reuses_rreq_forward_range);
    return UNITY_END();
}
