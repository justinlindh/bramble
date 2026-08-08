#include "unity.h"
#include "../components/routing/routing.c"

static neighbor_table_t tbl;

void setUp(void) { neighbor_init(&tbl); }
void tearDown(void) {}

void test_neighbor_init_empty(void) { TEST_ASSERT_EQUAL(0, neighbor_count(&tbl)); }

void test_neighbor_add_and_lookup(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(-70, e->rssi);
    TEST_ASSERT_EQUAL(8, e->snr);
    TEST_ASSERT_EQUAL(1, neighbor_count(&tbl));
}

void test_neighbor_update_existing(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    neighbor_update(&tbl, 0xAABB, -80, 5, 0x1234, 2000);
    TEST_ASSERT_EQUAL(1, neighbor_count(&tbl));
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_EQUAL(-80, e->rssi);
    TEST_ASSERT_EQUAL(5, e->snr);
    TEST_ASSERT_EQUAL(2000, e->last_heard);
}

void test_neighbor_purge_expired(void) {
    neighbor_update(&tbl, 1, -70, 8, 0, 1000);
    neighbor_update(&tbl, 2, -70, 8, 0, 700000);
    neighbor_purge(&tbl, 700000);
    TEST_ASSERT_EQUAL(1, neighbor_count(&tbl));
    TEST_ASSERT_NULL(neighbor_lookup(&tbl, 1));
    TEST_ASSERT_NOT_NULL(neighbor_lookup(&tbl, 2));
}

void test_neighbor_full_evicts_oldest(void) {
    for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
        neighbor_update(&tbl, i + 1, -70, 8, 0, 1000 + i);
    }
    TEST_ASSERT_EQUAL(MAX_NEIGHBORS, neighbor_count(&tbl));
    /* Add one more; addr 1 (oldest at t=1000) should be evicted */
    neighbor_update(&tbl, 0xFF, -60, 10, 0, 5000);
    TEST_ASSERT_EQUAL(MAX_NEIGHBORS, neighbor_count(&tbl));
    TEST_ASSERT_NULL(neighbor_lookup(&tbl, 1));
    TEST_ASSERT_NOT_NULL(neighbor_lookup(&tbl, 0xFF));
}

void test_newly_admitted_is_true_only_for_the_beacon_that_created_the_entry(void) {
    int idx = neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    TEST_ASSERT_TRUE(neighbor_is_newly_admitted(&tbl, idx, 1000));

    /* The same peer beaconing again is a refresh, not an admission. */
    idx = neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 2000);
    TEST_ASSERT_FALSE(neighbor_is_newly_admitted(&tbl, idx, 2000));

    TEST_ASSERT_FALSE(neighbor_is_newly_admitted(&tbl, -1, 1000));
    TEST_ASSERT_FALSE(neighbor_is_newly_admitted(&tbl, MAX_NEIGHBORS, 1000));
    TEST_ASSERT_FALSE(neighbor_is_newly_admitted(NULL, 0, 1000));
}

void test_newly_admitted_sees_an_admission_that_had_to_evict(void) {
    /* The case a count comparison cannot see: the table is full, so admitting
     * an address reclaims another one's slot and the count never moves. */
    for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
        neighbor_update(&tbl, i + 1, -70, 8, 0, 1000 + i);
    }
    int before = neighbor_count(&tbl);

    int idx = neighbor_update(&tbl, 0xFF, -60, 10, 0, 5000);

    TEST_ASSERT_EQUAL(before, neighbor_count(&tbl)); /* the table did not grow */
    TEST_ASSERT_TRUE(neighbor_is_newly_admitted(&tbl, idx, 5000));
}

void test_neighbor_new_has_fresh_tenure(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(1, e->beacon_count);
    TEST_ASSERT_EQUAL(1000, e->first_seen_ms);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 0xAABB, 1000));
}

void test_neighbor_becomes_established_after_beacons_and_age(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 0);
    /* Only 2 beacons but plenty of age: not enough beacons yet */
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, ESTABLISHED_MIN_AGE_MS);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 0xAABB, ESTABLISHED_MIN_AGE_MS));

    /* Third beacon arrives right at the age threshold: now both conditions hold */
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, ESTABLISHED_MIN_AGE_MS);
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_EQUAL(3, e->beacon_count);
    TEST_ASSERT_TRUE(neighbor_is_established(&tbl, 0xAABB, ESTABLISHED_MIN_AGE_MS));
}

void test_neighbor_established_false_before_min_beacons(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 0);
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, ESTABLISHED_MIN_AGE_MS * 10);
    TEST_ASSERT_EQUAL(2, neighbor_lookup(&tbl, 0xAABB)->beacon_count);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 0xAABB, ESTABLISHED_MIN_AGE_MS * 10));
}

void test_neighbor_established_false_before_min_age(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 0);
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 100);
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 200);
    TEST_ASSERT_EQUAL(3, neighbor_lookup(&tbl, 0xAABB)->beacon_count);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 0xAABB, 200));
}

void test_neighbor_is_established_unknown_addr_false(void) {
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 0xDEAD, 1000000));
}

void test_neighbor_beacon_count_saturates(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 0);
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    e->beacon_count = 0xFFFF;
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 100);
    TEST_ASSERT_EQUAL(0xFFFF, e->beacon_count);
}

void test_neighbor_purge_then_reappear_resets_tenure(void) {
    neighbor_update(&tbl, 1, -70, 8, 0, 1000);
    neighbor_update(&tbl, 1, -70, 8, 0, 1000 + ESTABLISHED_MIN_AGE_MS);
    neighbor_update(&tbl, 1, -70, 8, 0, 1000 + ESTABLISHED_MIN_AGE_MS);
    TEST_ASSERT_TRUE(neighbor_is_established(&tbl, 1, 1000 + ESTABLISHED_MIN_AGE_MS));

    /* Age it out past NEIGHBOR_EXPIRY_MS so purge evicts it */
    uint32_t purge_time = 1000 + ESTABLISHED_MIN_AGE_MS + NEIGHBOR_EXPIRY_MS;
    neighbor_purge(&tbl, purge_time);
    TEST_ASSERT_NULL(neighbor_lookup(&tbl, 1));

    /* Reappears: tenure must start fresh, not carry over */
    neighbor_update(&tbl, 1, -70, 8, 0, purge_time);
    neighbor_entry_t* e = neighbor_lookup(&tbl, 1);
    TEST_ASSERT_EQUAL(1, e->beacon_count);
    TEST_ASSERT_EQUAL(purge_time, e->first_seen_ms);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 1, purge_time));
}

void test_neighbor_touch_refreshes_liveness_and_signal(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    TEST_ASSERT_TRUE(neighbor_touch(&tbl, 0xAABB, -55, 11, 90000));
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_EQUAL(90000, e->last_heard);
    TEST_ASSERT_EQUAL(-55, e->rssi);
    TEST_ASSERT_EQUAL(11, e->snr);
}

void test_neighbor_touch_leaves_tenure_alone(void) {
    /* Tenure (anti-Sybil) stays beacon-gated: a peer that only ever sends us
     * data must not accrue the beacon_count that neighbor_is_established
     * requires, and must not have its first_seen_ms tenure clock reset. */
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    for (int i = 0; i < 10; i++) {
        neighbor_touch(&tbl, 0xAABB, -70, 8, 2000 + (uint32_t)i);
    }
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_EQUAL(1, e->beacon_count);
    TEST_ASSERT_EQUAL(1000, e->first_seen_ms);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 0xAABB, 1000 + ESTABLISHED_MIN_AGE_MS));
}

void test_neighbor_touch_ignores_unknown_addr(void) {
    /* Only beacons admit a peer to the table. A touch never creates an entry,
     * so a data frame claiming an unheard prev_hop cannot conjure a neighbor. */
    TEST_ASSERT_FALSE(neighbor_touch(&tbl, 0xDEAD, -60, 9, 5000));
    TEST_ASSERT_EQUAL(0, neighbor_count(&tbl));
    TEST_ASSERT_NULL(neighbor_lookup(&tbl, 0xDEAD));
}

void test_neighbor_touch_keeps_an_active_peer_from_being_purged(void) {
    /* The user-visible point of the touch: a peer we are actively exchanging
     * data with stays listed even if its beacons are being stretched or lost. */
    neighbor_update(&tbl, 1, -70, 8, 0, 1000);
    uint32_t t = 1000 + NEIGHBOR_EXPIRY_MS - 1;
    neighbor_touch(&tbl, 1, -70, 8, t);
    neighbor_purge(&tbl, t + 1);
    TEST_ASSERT_NOT_NULL(neighbor_lookup(&tbl, 1));
}

void test_neighbor_touch_never_moves_last_heard_backwards(void) {
    /* Frames can be dispatched out of order relative to the sample that
     * stamped last_heard; an older timestamp must not un-age a peer. */
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 50000);
    TEST_ASSERT_FALSE(neighbor_touch(&tbl, 0xAABB, -55, 11, 40000));
    neighbor_entry_t* e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_EQUAL(50000, e->last_heard);
    TEST_ASSERT_EQUAL(-70, e->rssi);
}

void test_link_penalty_excellent(void) {
    uint8_t p = compute_link_penalty(-60, 10);
    TEST_ASSERT_LESS_OR_EQUAL(5, p);
}

void test_link_penalty_marginal(void) {
    uint8_t p = compute_link_penalty(-120, -5);
    TEST_ASSERT_GREATER_OR_EQUAL(30, p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_neighbor_init_empty);
    RUN_TEST(test_neighbor_add_and_lookup);
    RUN_TEST(test_neighbor_update_existing);
    RUN_TEST(test_neighbor_purge_expired);
    RUN_TEST(test_neighbor_full_evicts_oldest);
    RUN_TEST(test_newly_admitted_is_true_only_for_the_beacon_that_created_the_entry);
    RUN_TEST(test_newly_admitted_sees_an_admission_that_had_to_evict);
    RUN_TEST(test_neighbor_new_has_fresh_tenure);
    RUN_TEST(test_neighbor_becomes_established_after_beacons_and_age);
    RUN_TEST(test_neighbor_established_false_before_min_beacons);
    RUN_TEST(test_neighbor_established_false_before_min_age);
    RUN_TEST(test_neighbor_is_established_unknown_addr_false);
    RUN_TEST(test_neighbor_beacon_count_saturates);
    RUN_TEST(test_neighbor_purge_then_reappear_resets_tenure);
    RUN_TEST(test_neighbor_touch_refreshes_liveness_and_signal);
    RUN_TEST(test_neighbor_touch_leaves_tenure_alone);
    RUN_TEST(test_neighbor_touch_ignores_unknown_addr);
    RUN_TEST(test_neighbor_touch_keeps_an_active_peer_from_being_purged);
    RUN_TEST(test_neighbor_touch_never_moves_last_heard_backwards);
    RUN_TEST(test_link_penalty_excellent);
    RUN_TEST(test_link_penalty_marginal);
    return UNITY_END();
}
