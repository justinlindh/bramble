#include "unity.h"
#include "../components/security/security.c"

void setUp(void) {}
void tearDown(void) {}

void test_rreq_rate_first_allowed(void) {
    rreq_rate_limiter_t rl;
    rreq_rate_init(&rl);
    TEST_ASSERT_TRUE(rreq_rate_allow(&rl, 0xAA, 0xBB, 10000));
}

void test_rreq_rate_too_fast_rejected(void) {
    rreq_rate_limiter_t rl;
    rreq_rate_init(&rl);
    rreq_rate_allow(&rl, 0xAA, 0xBB, 10000);
    TEST_ASSERT_FALSE(rreq_rate_allow(&rl, 0xAA, 0xBB, 20000)); // 10s < 30s
}

void test_rreq_rate_after_cooldown_allowed(void) {
    rreq_rate_limiter_t rl;
    rreq_rate_init(&rl);
    rreq_rate_allow(&rl, 0xAA, 0xBB, 10000);
    TEST_ASSERT_TRUE(rreq_rate_allow(&rl, 0xAA, 0xBB, 50000)); // 40s > 30s
}

void test_sybil_rssi_cluster_detected(void) {
    // 3 nodes all within 3dB of each other → suspicious
    int8_t rssi[] = { -70, -71, -72, -50 };
    TEST_ASSERT_TRUE(sybil_check_rssi_cluster(rssi, 4));

    // All spread out → not suspicious
    int8_t rssi2[] = { -70, -50, -30 };
    TEST_ASSERT_FALSE(sybil_check_rssi_cluster(rssi2, 3));
}

void test_rreq_fwd_burst_then_rejected(void) {
    rreq_fwd_limiter_t rl;
    rreq_fwd_init(&rl, 0);
    for (int i = 0; i < RREQ_FWD_BURST; i++) {
        TEST_ASSERT_TRUE(rreq_fwd_allow(&rl, 0));
    }
    TEST_ASSERT_FALSE(rreq_fwd_allow(&rl, 0));
}

void test_rreq_fwd_refill_after_interval_allows_one(void) {
    rreq_fwd_limiter_t rl;
    rreq_fwd_init(&rl, 0);
    for (int i = 0; i < RREQ_FWD_BURST; i++) {
        TEST_ASSERT_TRUE(rreq_fwd_allow(&rl, 0));
    }
    TEST_ASSERT_FALSE(rreq_fwd_allow(&rl, RREQ_FWD_REFILL_MS - 1));
    TEST_ASSERT_TRUE(rreq_fwd_allow(&rl, RREQ_FWD_REFILL_MS));
    TEST_ASSERT_FALSE(rreq_fwd_allow(&rl, RREQ_FWD_REFILL_MS));
}

void test_rreq_fwd_long_idle_caps_at_burst(void) {
    rreq_fwd_limiter_t rl;
    rreq_fwd_init(&rl, 0);
    // Drain the initial burst so refill accrual (not the initial fill) is what's under test.
    for (int i = 0; i < RREQ_FWD_BURST; i++) {
        rreq_fwd_allow(&rl, 0);
    }
    uint32_t huge_idle = (uint32_t) RREQ_FWD_BURST * RREQ_FWD_REFILL_MS * 100;
    for (int i = 0; i < RREQ_FWD_BURST; i++) {
        TEST_ASSERT_TRUE(rreq_fwd_allow(&rl, huge_idle));
    }
    TEST_ASSERT_FALSE(rreq_fwd_allow(&rl, huge_idle));
}

void test_rreq_fwd_monotonic_flood_bounded_to_refill_rate(void) {
    rreq_fwd_limiter_t rl;
    rreq_fwd_init(&rl, 0);
    for (int i = 0; i < RREQ_FWD_BURST; i++) {
        rreq_fwd_allow(&rl, 0);
    }
    // Burst is drained. Flood one call per ms for 10 refill windows and count
    // how many are allowed: should be bounded to about 1 per RREQ_FWD_REFILL_MS.
    uint32_t windows = 10;
    uint32_t end_ms = windows * RREQ_FWD_REFILL_MS;
    int allowed = 0;
    for (uint32_t t = 1; t <= end_ms; t++) {
        if (rreq_fwd_allow(&rl, t)) {
            allowed++;
        }
    }
    TEST_ASSERT_EQUAL_INT((int) windows, allowed);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rreq_rate_first_allowed);
    RUN_TEST(test_rreq_rate_too_fast_rejected);
    RUN_TEST(test_rreq_rate_after_cooldown_allowed);
    RUN_TEST(test_sybil_rssi_cluster_detected);
    RUN_TEST(test_rreq_fwd_burst_then_rejected);
    RUN_TEST(test_rreq_fwd_refill_after_interval_allows_one);
    RUN_TEST(test_rreq_fwd_long_idle_caps_at_burst);
    RUN_TEST(test_rreq_fwd_monotonic_flood_bounded_to_refill_rate);
    return UNITY_END();
}
