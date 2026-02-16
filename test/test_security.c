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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rreq_rate_first_allowed);
    RUN_TEST(test_rreq_rate_too_fast_rejected);
    RUN_TEST(test_rreq_rate_after_cooldown_allowed);
    RUN_TEST(test_sybil_rssi_cluster_detected);
    return UNITY_END();
}
