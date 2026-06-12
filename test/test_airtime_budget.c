#include "unity.h"
#include "../components/airtime/airtime_budget.c"

void setUp(void) {}
void tearDown(void) {}

void test_budget_starts_full(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL], airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_CRITICAL], airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_BROADCAST], airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_RECEIPT], airtime_budget_remaining(&ab, AIRTIME_TIER_RECEIPT));
}

void test_continuous_refill_partial_interval(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);

    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 9000);
    uint32_t before = airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_BROADCAST] - 9000u, before);

    /* 30 minutes should refill ~50% of bucket capacity */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS / 2u);
    uint32_t after = airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_TRUE(after > before);
    TEST_ASSERT_TRUE(after <= ab.max_ms[AIRTIME_IDX_BROADCAST]);
}

void test_refill_clamps_to_max(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, AIRTIME_BUDGET_NORMAL_MS);

    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS * 10u);
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL], airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_critical_borrow_from_normal_still_works(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, ab.max_ms[AIRTIME_IDX_CRITICAL]);
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 1000u));
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 1000u);
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL] - 1000u,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_receipt_tier_isolated_from_broadcast(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_RECEIPT, ab.max_ms[AIRTIME_IDX_RECEIPT]);
    TEST_ASSERT_EQUAL_UINT32(0u, airtime_budget_remaining(&ab, AIRTIME_TIER_RECEIPT));
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_BROADCAST], airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

void test_adaptive_profile_small_mesh_relaxes_budgets(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    uint32_t base_bcast = ab.base_max_ms[AIRTIME_IDX_BROADCAST];
    uint32_t base_rcpt = ab.base_max_ms[AIRTIME_IDX_RECEIPT];

    airtime_budget_set_mesh_size(&ab, 5);
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_BROADCAST] > base_bcast);
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_RECEIPT] > base_rcpt);
}

void test_adaptive_profile_large_mesh_constrains_budgets(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    uint32_t base_bcast = ab.base_max_ms[AIRTIME_IDX_BROADCAST];
    uint32_t base_rcpt = ab.base_max_ms[AIRTIME_IDX_RECEIPT];

    airtime_budget_set_mesh_size(&ab, 80);
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_BROADCAST] < base_bcast);
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_RECEIPT] < base_rcpt);
}

void test_next_refill_is_zero_for_continuous_model(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    TEST_ASSERT_EQUAL_UINT32(0u, airtime_budget_next_refill_ms(&ab, 1234u));
}

/* Review finding (MAJOR, PR #82): CRITICAL borrowing from NORMAL was
 * unbounded, so a remote query_id-varying RREQ flood (forwarded on the
 * CRITICAL lane until 1.3 lands forward-side rate limiting) could drain
 * the local user-data budget. The borrow is now its own mini-bucket
 * capped at 25% of NORMAL's capacity/refill: relayed control can degrade
 * the data lane but never exhaust it. */
void test_critical_borrow_capped_at_quarter_of_normal(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);

    uint32_t normal_max = ab.max_ms[AIRTIME_IDX_NORMAL];
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, ab.max_ms[AIRTIME_IDX_CRITICAL]);

    uint64_t borrowed = 0;
    while (airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 100u)) {
        airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 100u);
        borrowed += 100u;
        TEST_ASSERT_TRUE(borrowed <= (uint64_t)normal_max / 4u);
    }

    /* NORMAL keeps at least 75% of its bucket for local data. */
    TEST_ASSERT_TRUE(airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL) >=
                     (normal_max * 3u) / 4u);
    /* And a normal-tier send still passes. */
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_NORMAL, 1000u));
}

void test_borrow_allowance_refills_over_time(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    uint32_t normal_max = ab.max_ms[AIRTIME_IDX_NORMAL];

    /* Exhaust CRITICAL and the borrow allowance. */
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, ab.max_ms[AIRTIME_IDX_CRITICAL]);
    while (airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 100u))
        airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 100u);
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 100u));

    /* Half an hour later some CRITICAL tokens are back; meanwhile the
     * borrow cap still protects NORMAL (it refilled too, but spend via
     * borrow in this window stays bounded by the allowance). */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS / 2u);
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 100u));
    TEST_ASSERT_TRUE(airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL) >=
                     (normal_max * 3u) / 4u);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_starts_full);
    RUN_TEST(test_continuous_refill_partial_interval);
    RUN_TEST(test_refill_clamps_to_max);
    RUN_TEST(test_critical_borrow_from_normal_still_works);
    RUN_TEST(test_critical_borrow_capped_at_quarter_of_normal);
    RUN_TEST(test_borrow_allowance_refills_over_time);
    RUN_TEST(test_receipt_tier_isolated_from_broadcast);
    RUN_TEST(test_adaptive_profile_small_mesh_relaxes_budgets);
    RUN_TEST(test_adaptive_profile_large_mesh_constrains_budgets);
    RUN_TEST(test_next_refill_is_zero_for_continuous_model);
    return UNITY_END();
}
