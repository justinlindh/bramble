#include "unity/unity.h"
#include "../components/bramble_probe/bramble_probe.c"

void setUp(void) {}
void tearDown(void) {}

void test_rate_limit_init_sets_full_bucket_and_metadata(void) {
    probe_rate_limit_t rl = {0};

    rate_limit_init(&rl, 3, 60000, 1234);

    TEST_ASSERT_EQUAL_UINT8(3, rl.tokens);
    TEST_ASSERT_EQUAL_UINT8(3, rl.max_tokens);
    TEST_ASSERT_EQUAL_UINT32(60000, rl.refill_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(1234, rl.last_refill_ms);
}

void test_rate_limit_refill_adds_tokens_without_losing_remainder(void) {
    probe_rate_limit_t rl = {0};
    rate_limit_init(&rl, 5, 1000, 0);
    rl.tokens = 0;

    rate_limit_refill(&rl, 2500);
    TEST_ASSERT_EQUAL_UINT8(2, rl.tokens);
    TEST_ASSERT_EQUAL_UINT32(2000, rl.last_refill_ms);

    /* Ensure leftover 500ms is preserved, not rounded away. */
    rate_limit_refill(&rl, 3000);
    TEST_ASSERT_EQUAL_UINT8(3, rl.tokens);
    TEST_ASSERT_EQUAL_UINT32(3000, rl.last_refill_ms);
}

void test_rate_limit_refill_caps_at_max_tokens(void) {
    probe_rate_limit_t rl = {0};
    rate_limit_init(&rl, 3, 1000, 0);
    rl.tokens = 1;

    rate_limit_refill(&rl, 10000);

    TEST_ASSERT_EQUAL_UINT8(3, rl.tokens);
    TEST_ASSERT_EQUAL_UINT32(10000, rl.last_refill_ms);
}

void test_rate_limit_try_succeeds_and_consumes_token(void) {
    probe_rate_limit_t rl = {0};
    rate_limit_init(&rl, 2, 1000, 0);

    TEST_ASSERT_TRUE(rate_limit_try(&rl, 0));
    TEST_ASSERT_EQUAL_UINT8(1, rl.tokens);
}

void test_rate_limit_try_fails_when_empty(void) {
    probe_rate_limit_t rl = {0};
    rate_limit_init(&rl, 1, 1000, 0);
    rl.tokens = 0;

    TEST_ASSERT_FALSE(rate_limit_try(&rl, 999));
    TEST_ASSERT_EQUAL_UINT8(0, rl.tokens);
}

void test_rate_limit_refill_small_intervals_boundary(void) {
    probe_rate_limit_t rl = {0};
    rate_limit_init(&rl, 5, 60000, 0);
    rl.tokens = 0;

    rate_limit_refill(&rl, 59999);
    TEST_ASSERT_EQUAL_UINT8(0, rl.tokens);
    TEST_ASSERT_EQUAL_UINT32(0, rl.last_refill_ms);

    rate_limit_refill(&rl, 60000);
    TEST_ASSERT_EQUAL_UINT8(1, rl.tokens);
    TEST_ASSERT_EQUAL_UINT32(60000, rl.last_refill_ms);
}

void test_rate_limit_burst_behavior_and_incremental_recovery(void) {
    probe_rate_limit_t rl = {0};
    rate_limit_init(&rl, 3, 60000, 0);

    TEST_ASSERT_TRUE(rate_limit_try(&rl, 0));
    TEST_ASSERT_TRUE(rate_limit_try(&rl, 1));
    TEST_ASSERT_TRUE(rate_limit_try(&rl, 2));
    TEST_ASSERT_FALSE(rate_limit_try(&rl, 3));

    /* One token recovered after one interval. */
    TEST_ASSERT_TRUE(rate_limit_try(&rl, 60000));
    TEST_ASSERT_FALSE(rate_limit_try(&rl, 60001));

    /* Two full intervals recover exactly two tokens. */
    rate_limit_refill(&rl, 180000);
    TEST_ASSERT_EQUAL_UINT8(2, rl.tokens);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rate_limit_init_sets_full_bucket_and_metadata);
    RUN_TEST(test_rate_limit_refill_adds_tokens_without_losing_remainder);
    RUN_TEST(test_rate_limit_refill_caps_at_max_tokens);
    RUN_TEST(test_rate_limit_try_succeeds_and_consumes_token);
    RUN_TEST(test_rate_limit_try_fails_when_empty);
    RUN_TEST(test_rate_limit_refill_small_intervals_boundary);
    RUN_TEST(test_rate_limit_burst_behavior_and_incremental_recovery);
    return UNITY_END();
}
