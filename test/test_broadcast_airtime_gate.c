#include "unity.h"
#include "airtime_budget.h"

/* We test the policy logic, not mesh_send_broadcast directly (that requires
   the full mesh_task runtime).  The contract is:
     - broadcast is allowed when airtime_budget_can_transmit(BROADCAST, est) is true
     - broadcast is denied when the budget is exhausted
     - budget adapts to mesh size                                              */

void setUp(void) {}
void tearDown(void) {}

/* A broadcast of ~50 bytes at SF10/125kHz ≈ 250ms airtime.
   Use a round number for test clarity. */
#define BROADCAST_AIRTIME_MS 250u

void test_broadcast_allowed_when_budget_available(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Fresh budget should allow a broadcast */
    TEST_ASSERT_TRUE(
        airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

void test_broadcast_denied_when_budget_exhausted(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Drain the entire broadcast budget */
    uint32_t remaining = airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, remaining);
    TEST_ASSERT_EQUAL_UINT32(0u, airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
    /* Now a broadcast should be denied */
    TEST_ASSERT_FALSE(
        airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

void test_broadcast_budget_refills_over_time(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    uint32_t remaining = airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, remaining);
    /* After half the refill interval, some budget should be restored */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS / 2u);
    TEST_ASSERT_TRUE(airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST) > 0u);
    TEST_ASSERT_TRUE(
        airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

void test_micro_mesh_gets_more_broadcast_budget(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Default (0 peers) uses micro profile */
    uint32_t micro_max = ab.max_ms[AIRTIME_IDX_BROADCAST];

    airtime_budget_t ab2;
    airtime_budget_init(&ab2, 0);
    airtime_budget_set_mesh_size(&ab2, 50);
    uint32_t large_max = ab2.max_ms[AIRTIME_IDX_BROADCAST];

    /* Micro mesh should have significantly more broadcast budget */
    TEST_ASSERT_TRUE(micro_max > large_max * 4u);
}

void test_rapid_broadcasts_exhaust_budget_not_arbitrary_limit(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Micro mesh: budget is 400% of 18000ms = 72000ms.
       At 250ms per broadcast, we should get 72000/250 = 288 broadcasts
       before exhaustion, far more than the old 3-burst limit. */
    int count = 0;
    while (airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS)) {
        airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS);
        count++;
        if (count > 500)
            break; /* safety valve */
    }
    /* Should allow many more than the old 3-burst limit */
    TEST_ASSERT_TRUE(count > 100);
    /* But should eventually exhaust */
    TEST_ASSERT_FALSE(
        airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_broadcast_allowed_when_budget_available);
    RUN_TEST(test_broadcast_denied_when_budget_exhausted);
    RUN_TEST(test_broadcast_budget_refills_over_time);
    RUN_TEST(test_micro_mesh_gets_more_broadcast_budget);
    RUN_TEST(test_rapid_broadcasts_exhaust_budget_not_arbitrary_limit);
    return UNITY_END();
}
