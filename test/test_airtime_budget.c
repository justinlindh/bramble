#include "unity.h"
#include "../components/airtime/airtime_budget.c"

void setUp(void) {}
void tearDown(void) {}

void test_budget_starts_full(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS, airtime_budget_remaining(&ab, 0));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS, airtime_budget_remaining(&ab, 1));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS, airtime_budget_remaining(&ab, 2));
}

void test_budget_debit_reduces(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, 1, 5000);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS - 5000, airtime_budget_remaining(&ab, 1));
}

void test_budget_refill_restores(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, 0, 10000);
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS, airtime_budget_remaining(&ab, 0));
}

void test_budget_blocks_when_empty(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, 0, AIRTIME_BUDGET_BROADCAST_MS);
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, 0, 1));
}

void test_critical_borrows_from_normal(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    // Exhaust critical budget
    airtime_budget_debit(&ab, 2, AIRTIME_BUDGET_CRITICAL_MS);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, 2));
    // Critical can borrow from normal
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, 2, 1000));
    // Normal alone cannot be borrowed for broadcast
    airtime_budget_debit(&ab, 0, AIRTIME_BUDGET_BROADCAST_MS);
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, 0, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_starts_full);
    RUN_TEST(test_budget_debit_reduces);
    RUN_TEST(test_budget_refill_restores);
    RUN_TEST(test_budget_blocks_when_empty);
    RUN_TEST(test_critical_borrows_from_normal);
    return UNITY_END();
}
