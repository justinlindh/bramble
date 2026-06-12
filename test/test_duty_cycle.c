#include "unity.h"
#include <stdio.h>
#include "../components/airtime/airtime_budget.c"

/*
 * Duty-cycle cap tests (DES-8).
 *
 * EU868 (ETSI EN 300.220) allows 1% duty cycle: at most 36000 ms of TX per
 * hour. The budget refills each tier to max_ms once per
 * AIRTIME_REFILL_INTERVAL_MS (one hour), so capping the SUM of tier maxima
 * at duty_pct% of the interval caps steady-state TX time at the regulatory
 * limit. US915 has no duty limit (100%, not enforced) and must be unchanged.
 */

void setUp(void) {}
void tearDown(void) {}

#define EU868_CAP_MS ((AIRTIME_REFILL_INTERVAL_MS / 100u) * 1u) /* 36000 */
/* Burst capacity equals hourly refill in this bucket model, so both must
 * fit in half the cap for ANY 1-hour window to stay within the limit. */
#define EU868_WINDOW_TARGET_MS (EU868_CAP_MS / 2u) /* 18000 */

static uint32_t sum_max(const airtime_budget_t* ab) {
    uint32_t total = 0;
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++)
        total += ab->max_ms[i];
    return total;
}

static uint32_t sum_tokens(const airtime_budget_t* ab) {
    uint32_t total = 0;
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++)
        total += ab->tokens_ms[i];
    return total;
}

void test_eu868_cap_applies_at_init_profile(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_set_duty_cap(&ab, 1, true);
    /* capacity + hourly refill = 2 * sum(max) must fit the cap */
    TEST_ASSERT_TRUE(sum_max(&ab) <= EU868_WINDOW_TARGET_MS);
    TEST_ASSERT_TRUE(sum_tokens(&ab) <= EU868_WINDOW_TARGET_MS);
}

void test_eu868_cap_holds_across_all_mesh_profiles(void) {
    /* The micro-mesh profile (<=8 peers) scales budgets up to ~6.2% duty.
     * The cap must hold for every peer-count profile. */
    const uint8_t peer_counts[] = {0, 5, 8, 9, 12, 15, 16, 20, 40, 41, 50, 255};
    for (size_t i = 0; i < sizeof(peer_counts); i++) {
        airtime_budget_t ab;
        airtime_budget_init(&ab, 0);
        airtime_budget_set_duty_cap(&ab, 1, true);
        airtime_budget_set_mesh_size(&ab, peer_counts[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "peer_count=%u sum=%u", (unsigned)peer_counts[i],
                 (unsigned)sum_max(&ab));
        TEST_ASSERT_TRUE_MESSAGE(sum_max(&ab) <= EU868_WINDOW_TARGET_MS, msg);
    }
}

void test_eu868_hourly_refill_bounded_by_cap(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_set_duty_cap(&ab, 1, true);

    /* Drain everything, then refill a full hour: total tokens added must
     * fit the any-window budget alongside the burst capacity. */
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++)
        ab.tokens_ms[i] = 0;
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS);
    TEST_ASSERT_TRUE(sum_tokens(&ab) <= EU868_WINDOW_TARGET_MS);
}

void test_eu868_any_window_spend_bounded(void) {
    /* ETSI evaluates ANY 1-hour window. Worst case for a token bucket:
     * idle long enough to fill the bucket, then spend the full bucket at
     * the window start and drain every refill for the rest of the hour.
     * Total TX inside that window must still be <= 1% (36000 ms). */
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_set_duty_cap(&ab, 1, true);

    /* Idle hour: bucket reaches capacity. */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS);

    /* Hostile window: drain everything available, every minute. */
    uint64_t spent = 0;
    const uint32_t t0 = AIRTIME_REFILL_INTERVAL_MS;
    for (uint32_t dt = 0; dt <= AIRTIME_REFILL_INTERVAL_MS; dt += 60000u) {
        airtime_budget_refill(&ab, t0 + dt);
        for (int i = 0; i < AIRTIME_TIER_COUNT; i++) {
            spent += ab.tokens_ms[i];
            ab.tokens_ms[i] = 0;
        }
    }
    TEST_ASSERT_TRUE(spent <= EU868_CAP_MS);
}

void test_us915_unenforced_unchanged(void) {
    airtime_budget_t plain, us;
    airtime_budget_init(&plain, 0);
    airtime_budget_init(&us, 0);
    airtime_budget_set_duty_cap(&us, 100, false);

    for (int i = 0; i < AIRTIME_TIER_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(plain.max_ms[i], us.max_ms[i]);
        TEST_ASSERT_EQUAL_UINT32(plain.tokens_ms[i], us.tokens_ms[i]);
    }

    airtime_budget_set_mesh_size(&plain, 5);
    airtime_budget_set_mesh_size(&us, 5);
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++)
        TEST_ASSERT_EQUAL_UINT32(plain.max_ms[i], us.max_ms[i]);
}

void test_enforced_100pct_is_a_noop_cap(void) {
    /* Even "enforced" 100% should not shrink anything: the default budgets
     * sum to well under 100% of an hour. */
    airtime_budget_t plain, capped;
    airtime_budget_init(&plain, 0);
    airtime_budget_init(&capped, 0);
    airtime_budget_set_duty_cap(&capped, 100, true);
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++)
        TEST_ASSERT_EQUAL_UINT32(plain.max_ms[i], capped.max_ms[i]);
}

void test_cap_preserves_tier_proportions(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_set_duty_cap(&ab, 1, true);
    airtime_budget_set_mesh_size(&ab, 20); /* baseline profile: 100% scaling */

    /* Baseline maxes: critical 36000, normal 18000, broadcast 18000,
     * receipt 12000 (sum 84000). Capped to 36000 total, ratios hold. */
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_CRITICAL] > ab.max_ms[AIRTIME_IDX_NORMAL]);
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_NORMAL] > ab.max_ms[AIRTIME_IDX_RECEIPT]);
    /* Critical : receipt = 3 : 1 within integer rounding */
    TEST_ASSERT_TRUE(ab.max_ms[AIRTIME_IDX_CRITICAL] >= 2u * ab.max_ms[AIRTIME_IDX_RECEIPT]);
}

void test_no_tier_zeroed_by_cap(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_set_duty_cap(&ab, 1, true);
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++)
        TEST_ASSERT_TRUE(ab.max_ms[i] >= 1u);
}

void test_critical_borrow_cannot_exceed_cap(void) {
    /* Borrowing spends NORMAL tokens, so the total spendable pool is still
     * the sum of tier buckets, which the cap bounds. */
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    airtime_budget_set_duty_cap(&ab, 1, true);

    uint64_t spent = 0;
    while (airtime_budget_can_transmit(&ab, AIRTIME_TIER_CRITICAL, 100u)) {
        airtime_budget_debit(&ab, AIRTIME_TIER_CRITICAL, 100u);
        spent += 100u;
        TEST_ASSERT_TRUE(spent <= EU868_CAP_MS);
    }
    TEST_ASSERT_TRUE(spent <= EU868_CAP_MS);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_eu868_cap_applies_at_init_profile);
    RUN_TEST(test_eu868_cap_holds_across_all_mesh_profiles);
    RUN_TEST(test_eu868_hourly_refill_bounded_by_cap);
    RUN_TEST(test_eu868_any_window_spend_bounded);
    RUN_TEST(test_us915_unenforced_unchanged);
    RUN_TEST(test_enforced_100pct_is_a_noop_cap);
    RUN_TEST(test_cap_preserves_tier_proportions);
    RUN_TEST(test_no_tier_zeroed_by_cap);
    RUN_TEST(test_critical_borrow_cannot_exceed_cap);
    return UNITY_END();
}
