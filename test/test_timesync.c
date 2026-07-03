#include "unity.h"
#include "timesync.h"

void setUp(void) {}
void tearDown(void) {}

/* ── Basic sync behavior ────────────────────────────────────────── */

void test_single_source_does_not_commit(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* One source is not enough — CORROBORATION_REQUIRED = 3 */
    int rc = timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    TEST_ASSERT_EQUAL_INT(0, rc);  /* accepted, not committed */
    TEST_ASSERT_FALSE(ts.synchronized);
}

void test_two_sources_does_not_commit(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    int rc = timesync_handle_sync(&ts, 1120, 1, 0xBBBB, 1000);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(ts.synchronized);
}

void test_three_distinct_sources_commits(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    timesync_handle_sync(&ts, 1120, 1, 0xBBBB, 1000);
    int rc = timesync_handle_sync(&ts, 1080, 1, 0xCCCC, 1000);

    TEST_ASSERT_EQUAL_INT(1, rc);  /* committed */
    TEST_ASSERT_TRUE(ts.synchronized);
    TEST_ASSERT_EQUAL_UINT8(2, timesync_get_stratum(&ts));  /* best source 1 + 1 */
}

void test_duplicate_source_does_not_count_as_distinct(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    timesync_handle_sync(&ts, 1120, 1, 0xBBBB, 1000);
    /* Same source as first — should update, not add distinct */
    int rc = timesync_handle_sync(&ts, 1090, 1, 0xAAAA, 1000);
    TEST_ASSERT_EQUAL_INT(0, rc);  /* still only 2 distinct sources */
    TEST_ASSERT_FALSE(ts.synchronized);

    /* Third distinct source commits */
    rc = timesync_handle_sync(&ts, 1080, 1, 0xCCCC, 1000);
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_TRUE(ts.synchronized);
}

/* ── Weighted average ───────────────────────────────────────────── */

void test_weighted_average_prefers_better_stratum(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* Stratum 6 (weight 1), stratum 1 (weight 6), stratum 2 (weight 5) */
    timesync_handle_sync(&ts, 1450, 6, 0xAAAA, 1000);  /* offset +450 */
    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000);  /* offset +100 */
    int rc = timesync_handle_sync(&ts, 1200, 2, 0xCCCC, 1000);  /* offset +200 */
    TEST_ASSERT_EQUAL_INT(1, rc);

    /* Weighted: (450*1 + 100*6 + 200*5) / (1+6+5) = (450+600+1000)/12 = 170 */
    TEST_ASSERT_EQUAL_INT64(170, ts.offset_ms);
    /* Best stratum is 1, so our stratum is 2 */
    TEST_ASSERT_EQUAL_UINT8(2, timesync_get_stratum(&ts));
}

/* ── Rejection ──────────────────────────────────────────────────── */

void test_rejects_worse_stratum_when_synchronized(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* Sync from 3 sources at stratum 1 → our stratum = 2 */
    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xCCCC, 1000);
    TEST_ASSERT_TRUE(ts.synchronized);
    TEST_ASSERT_EQUAL_UINT8(2, ts.stratum);

    /* Stratum 2 is not strictly better than our 2 */
    int rc = timesync_handle_sync(&ts, 1200, 2, 0xDDDD, 2000);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_rejects_large_shift_when_synchronized(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* Sync with offset ~+100 */
    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xCCCC, 1000);
    TEST_ASSERT_TRUE(ts.synchronized);

    /* Propose offset +5000 — shift of ~4900, exceeds MAX_TIME_SHIFT_MS (2000) */
    int rc = timesync_handle_sync(&ts, 6000, 0, 0xDDDD, 1000);
    TEST_ASSERT_EQUAL_INT(-2, rc);
}

/* ── Aging ──────────────────────────────────────────────────────── */

void test_stale_entries_expire(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* Two entries at t=1000 */
    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000);

    /* Third entry at t=200000 — first two are now >180s old and should be purged */
    int rc = timesync_handle_sync(&ts, 200100, 1, 0xCCCC, 200000);
    TEST_ASSERT_EQUAL_INT(0, rc);  /* only 1 non-expired source (0xCCCC) */
    TEST_ASSERT_FALSE(ts.synchronized);
}

/* ── Network time output ────────────────────────────────────────── */

void test_get_network_time(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* Force sync for this test */
    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000);
    timesync_handle_sync(&ts, 1100, 1, 0xCCCC, 1000);
    TEST_ASSERT_TRUE(ts.synchronized);

    /* offset should be ~100; network_time at local_now=2000 = 2100 */
    int64_t nt = timesync_get_network_time(&ts, 2000);
    TEST_ASSERT_EQUAL_INT64(2100, nt);
}

/* ── Bootstrap clamp (NEW-SEC-4, Task 3.5, STAGED) ─────────────────── */

/*
 * The key regression case. Two honest sources have already agreed on an
 * offset near 100 (well within MAX_TIME_SHIFT_MS of each other); a third
 * proposal wildly inconsistent with that (an attacker, or a misconfigured
 * node) must be rejected as an outlier rather than silently averaged into
 * the quorum, since without this fix a lone bad proposal reaching
 * CORROBORATION_REQUIRED distinct sources would commit immediately.
 */
void test_rejects_offset_inconsistent_with_pending_quorum(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000); /* offset ~100 */
    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000); /* offset ~100 */

    /* Wildly inconsistent: offset ~49000, far beyond MAX_TIME_SHIFT_MS
     * (2000) from the ~100 consensus already pending. */
    int rc = timesync_handle_sync(&ts, 50000, 1, 0xCCCC, 1000);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_FALSE(ts.synchronized);

    /* A genuinely consistent third source still completes the quorum and
     * commits normally: the fix rejects disagreement, not corroboration
     * itself. */
    rc = timesync_handle_sync(&ts, 1080, 1, 0xDDDD, 1000);
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_TRUE(ts.synchronized);
}

/* The very first pending entry has nothing to compare against yet and must
 * always be admitted, no matter how far from zero: remote_time_ms is real
 * network time while local_now_ms is raw uptime since boot, so every
 * legitimate first proposal is naturally far from a zero baseline. */
void test_first_pending_entry_always_admitted_regardless_of_magnitude(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    int rc = timesync_handle_sync(&ts, 1700000000000LL, 1, 0xAAAA, 5000);
    TEST_ASSERT_EQUAL_INT(0, rc); /* accepted (pending), not yet committed */
    TEST_ASSERT_EQUAL_INT(1, ts.pending_count);
}

void test_is_confident_false_until_corroborated(void) {
    timesync_state_t ts;
    timesync_init(&ts);
    TEST_ASSERT_FALSE(timesync_is_confident(&ts));

    timesync_handle_sync(&ts, 1100, 1, 0xAAAA, 1000);
    TEST_ASSERT_FALSE(timesync_is_confident(&ts));

    timesync_handle_sync(&ts, 1100, 1, 0xBBBB, 1000);
    TEST_ASSERT_FALSE(timesync_is_confident(&ts));

    timesync_handle_sync(&ts, 1100, 1, 0xCCCC, 1000);
    TEST_ASSERT_TRUE(timesync_is_confident(&ts));
}

/* ── Pool overflow ──────────────────────────────────────────────── */

void test_pool_overflow_evicts_oldest(void) {
    timesync_state_t ts;
    timesync_init(&ts);

    /* Fill pool with 8 entries from different sources at different times */
    for (int i = 0; i < PENDING_POOL_SIZE; i++) {
        timesync_handle_sync(&ts, 1100 + i, 1, 0x1000 + i, 1000 + i);
    }
    TEST_ASSERT_EQUAL_INT(PENDING_POOL_SIZE, ts.pending_count);

    /* 9th entry should evict the oldest (timestamp 1000, addr 0x1000) */
    timesync_handle_sync(&ts, 1200, 1, 0x2000, 2000);
    TEST_ASSERT_EQUAL_INT(PENDING_POOL_SIZE, ts.pending_count);

    /* Verify oldest was evicted: 0x1000 should be gone */
    bool found_0x1000 = false;
    bool found_0x2000 = false;
    for (int i = 0; i < ts.pending_count; i++) {
        if (ts.pending[i].source_addr == 0x1000) found_0x1000 = true;
        if (ts.pending[i].source_addr == 0x2000) found_0x2000 = true;
    }
    TEST_ASSERT_FALSE(found_0x1000);
    TEST_ASSERT_TRUE(found_0x2000);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_source_does_not_commit);
    RUN_TEST(test_two_sources_does_not_commit);
    RUN_TEST(test_three_distinct_sources_commits);
    RUN_TEST(test_duplicate_source_does_not_count_as_distinct);
    RUN_TEST(test_weighted_average_prefers_better_stratum);
    RUN_TEST(test_rejects_worse_stratum_when_synchronized);
    RUN_TEST(test_rejects_large_shift_when_synchronized);
    RUN_TEST(test_stale_entries_expire);
    RUN_TEST(test_get_network_time);
    RUN_TEST(test_pool_overflow_evicts_oldest);
    RUN_TEST(test_rejects_offset_inconsistent_with_pending_quorum);
    RUN_TEST(test_first_pending_entry_always_admitted_regardless_of_magnitude);
    RUN_TEST(test_is_confident_false_until_corroborated);
    return UNITY_END();
}
