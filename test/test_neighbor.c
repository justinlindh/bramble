#include "unity.h"
#include "../components/routing/routing.c"

static neighbor_table_t tbl;

void setUp(void) { neighbor_init(&tbl); }
void tearDown(void) {}

void test_neighbor_init_empty(void) {
    TEST_ASSERT_EQUAL(0, neighbor_count(&tbl));
}

void test_neighbor_add_and_lookup(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    neighbor_entry_t *e = neighbor_lookup(&tbl, 0xAABB);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(-70, e->rssi);
    TEST_ASSERT_EQUAL(8, e->snr);
    TEST_ASSERT_EQUAL(1, neighbor_count(&tbl));
}

void test_neighbor_update_existing(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    neighbor_update(&tbl, 0xAABB, -80, 5, 0x1234, 2000);
    TEST_ASSERT_EQUAL(1, neighbor_count(&tbl));
    neighbor_entry_t *e = neighbor_lookup(&tbl, 0xAABB);
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

void test_neighbor_new_has_fresh_tenure(void) {
    neighbor_update(&tbl, 0xAABB, -70, 8, 0x1234, 1000);
    neighbor_entry_t *e = neighbor_lookup(&tbl, 0xAABB);
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
    neighbor_entry_t *e = neighbor_lookup(&tbl, 0xAABB);
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
    neighbor_entry_t *e = neighbor_lookup(&tbl, 0xAABB);
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
    neighbor_entry_t *e = neighbor_lookup(&tbl, 1);
    TEST_ASSERT_EQUAL(1, e->beacon_count);
    TEST_ASSERT_EQUAL(purge_time, e->first_seen_ms);
    TEST_ASSERT_FALSE(neighbor_is_established(&tbl, 1, purge_time));
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
    RUN_TEST(test_neighbor_new_has_fresh_tenure);
    RUN_TEST(test_neighbor_becomes_established_after_beacons_and_age);
    RUN_TEST(test_neighbor_established_false_before_min_beacons);
    RUN_TEST(test_neighbor_established_false_before_min_age);
    RUN_TEST(test_neighbor_is_established_unknown_addr_false);
    RUN_TEST(test_neighbor_beacon_count_saturates);
    RUN_TEST(test_neighbor_purge_then_reappear_resets_tenure);
    RUN_TEST(test_link_penalty_excellent);
    RUN_TEST(test_link_penalty_marginal);
    return UNITY_END();
}
