#include "unity.h"
#include "airtime_budget.h"

void setUp(void) {}
void tearDown(void) {}

void test_budget_math_and_remaining_by_tier(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 1000);

    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));

    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, 25);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS - 25,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_refill_restores_budgets_after_interval(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 1000);

    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, AIRTIME_BUDGET_NORMAL_MS);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));

    airtime_budget_refill(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS - 1);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));

    airtime_budget_refill(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_critical_overdraft_borrows_from_normal(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);

    uint32_t borrow_amount = AIRTIME_BUDGET_CRITICAL_MS + 10;
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, borrow_amount));

    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, borrow_amount);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS - 10,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_math_and_remaining_by_tier);
    RUN_TEST(test_refill_restores_budgets_after_interval);
    RUN_TEST(test_critical_overdraft_borrows_from_normal);
    return UNITY_END();
}
