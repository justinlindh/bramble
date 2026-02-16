#include "unity.h"
#include "../components/dedup/dedup.c"
#include "../components/routing/channel_flood.c"

static dedup_buffer_t dedup;

void setUp(void) { dedup_init(&dedup); }
void tearDown(void) {}

void test_flood_relay_valid(void) {
    flood_decision_t d = channel_flood_decide(4, 0x1234, &dedup);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(3, d.new_hop_limit);
    TEST_ASSERT_TRUE(d.jitter_ms >= 50 && d.jitter_ms <= 300);
}

void test_flood_stop_at_hop_1(void) {
    flood_decision_t d = channel_flood_decide(1, 0x1234, &dedup);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_flood_dedup_prevents_relay(void) {
    flood_decision_t d1 = channel_flood_decide(4, 0xAAAA, &dedup);
    TEST_ASSERT_TRUE(d1.should_relay);
    flood_decision_t d2 = channel_flood_decide(4, 0xAAAA, &dedup);
    TEST_ASSERT_FALSE(d2.should_relay);
}

void test_flood_jitter_range(void) {
    for (int i = 0; i < 100; i++) {
        dedup_init(&dedup); /* reset so each gets through dedup */
        flood_decision_t d = channel_flood_decide(4, 0x1000 + i, &dedup);
        TEST_ASSERT_TRUE(d.should_relay);
        TEST_ASSERT_TRUE(d.jitter_ms >= 50);
        TEST_ASSERT_TRUE(d.jitter_ms <= 300);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_flood_relay_valid);
    RUN_TEST(test_flood_stop_at_hop_1);
    RUN_TEST(test_flood_dedup_prevents_relay);
    RUN_TEST(test_flood_jitter_range);
    return UNITY_END();
}
