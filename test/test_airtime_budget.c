#include "unity.h"
#include "../components/airtime/airtime_budget.c"

void setUp(void) {}
void tearDown(void) {}

/* ── Init ────────────────────────────────────────────────────────────── */

void test_budget_starts_full(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

/* ── Debit ───────────────────────────────────────────────────────────── */

void test_debit_normal(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, 5000);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS - 5000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    /* Other tiers unaffected */
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

void test_debit_broadcast(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 3000);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS - 3000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

void test_debit_critical(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 10000);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS - 10000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
}

void test_debit_clamps_to_zero(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Debit more than available */
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, AIRTIME_BUDGET_BROADCAST_MS + 5000);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

/* ── Can transmit ────────────────────────────────────────────────────── */

void test_can_transmit_when_full(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_NORMAL, 1000));
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 1000));
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, 1000));
}

void test_blocks_when_empty(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, AIRTIME_BUDGET_BROADCAST_MS);
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, 1));
}

void test_critical_borrows_from_normal(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Exhaust critical */
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, AIRTIME_BUDGET_CRITICAL_MS);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    /* Critical can still transmit by borrowing from normal */
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 1000));
    /* Actually debit — should take from normal */
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 1000);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS - 1000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
}

void test_normal_cannot_borrow(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, AIRTIME_BUDGET_NORMAL_MS);
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_NORMAL, 1));
}

void test_broadcast_cannot_borrow(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, AIRTIME_BUDGET_BROADCAST_MS);
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, 1));
}

/* ── Refill ──────────────────────────────────────────────────────────── */

void test_refill_restores_all(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_NORMAL, 10000);
    airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 20000);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 15000);
    /* Before interval — no refill */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS - 1);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS - 10000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    /* At interval — full refill */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

void test_refill_no_early(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 1000);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 5000);
    /* Half the interval — should NOT refill */
    airtime_budget_refill(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS / 2);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS - 5000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

/* ── Next refill ─────────────────────────────────────────────────────── */

void test_next_refill_ms(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 1000);
    /* At init time, next refill is the full interval away */
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_REFILL_INTERVAL_MS,
                             airtime_budget_next_refill_ms(&ab, 1000));
    /* Halfway through */
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_REFILL_INTERVAL_MS / 2,
                             airtime_budget_next_refill_ms(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS / 2));
    /* Past due */
    TEST_ASSERT_EQUAL_UINT32(0,
                             airtime_budget_next_refill_ms(&ab, 1000 + AIRTIME_REFILL_INTERVAL_MS + 1));
}

/* ── Tier isolation ──────────────────────────────────────────────────── */

void test_tiers_are_independent(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Exhaust broadcast, verify others untouched */
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, AIRTIME_BUDGET_BROADCAST_MS);
    TEST_ASSERT_EQUAL_UINT32(0, airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_NORMAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_CRITICAL_MS,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_CRITICAL));
}

/* ── Multiple debits ─────────────────────────────────────────────────── */

void test_multiple_debits_accumulate(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 1000);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 2000);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, 3000);
    TEST_ASSERT_EQUAL_UINT32(AIRTIME_BUDGET_BROADCAST_MS - 6000,
                             airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_starts_full);
    RUN_TEST(test_debit_normal);
    RUN_TEST(test_debit_broadcast);
    RUN_TEST(test_debit_critical);
    RUN_TEST(test_debit_clamps_to_zero);
    RUN_TEST(test_can_transmit_when_full);
    RUN_TEST(test_blocks_when_empty);
    RUN_TEST(test_critical_borrows_from_normal);
    RUN_TEST(test_normal_cannot_borrow);
    RUN_TEST(test_broadcast_cannot_borrow);
    RUN_TEST(test_refill_restores_all);
    RUN_TEST(test_refill_no_early);
    RUN_TEST(test_next_refill_ms);
    RUN_TEST(test_tiers_are_independent);
    RUN_TEST(test_multiple_debits_accumulate);
    return UNITY_END();
}
