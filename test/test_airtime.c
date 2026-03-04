#include "unity.h"
#include "airtime_budget.h"

void setUp(void) {}
void tearDown(void) {}

void test_budget_math_and_remaining_by_tier(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 1000);

    /* Init applies profile(0): assert against active caps, not legacy constants. */
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL],
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_CRITICAL],
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));

    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, 25);
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL] - 25,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_refill_is_continuous_not_stepwise(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 1000);

    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, ab.max_ms[AIRTIME_IDX_NORMAL]);
    TEST_ASSERT_EQUAL_UINT32(0u, airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));

    /* Continuous model: even partial interval accrues tokens. */
    airtime_budget_refill(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS - 1u);
    TEST_ASSERT_TRUE(airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL) > 0u);

    airtime_budget_refill(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS);
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL],
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_critical_overdraft_borrows_from_normal(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);

    uint32_t borrow_amount = ab.max_ms[AIRTIME_IDX_CRITICAL] + 10u;
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, borrow_amount));

    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, borrow_amount);
    TEST_ASSERT_EQUAL_UINT32(0u, airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL_UINT32(ab.max_ms[AIRTIME_IDX_NORMAL] - 10u,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_math_and_remaining_by_tier);
    RUN_TEST(test_refill_is_continuous_not_stepwise);
    RUN_TEST(test_critical_overdraft_borrows_from_normal);
    return UNITY_END();
}
