#include "unity.h"
#include "../components/dedup/dedup.c"

static dedup_buffer_t buf;

void setUp(void) { dedup_init(&buf); }
void tearDown(void) {}

void test_init_count_zero(void) { TEST_ASSERT_EQUAL_INT(0, dedup_count(&buf)); }

void test_first_packet_not_duplicate(void) {
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 1000, 0));
    TEST_ASSERT_EQUAL_INT(1, dedup_count(&buf));
}

void test_same_packet_is_duplicate(void) {
    dedup_check_and_add(&buf, 1000, 0);
    TEST_ASSERT_TRUE(dedup_check_and_add(&buf, 1000, 100));
    TEST_ASSERT_EQUAL_INT(1, dedup_count(&buf));
}

void test_different_packets_not_duplicates(void) {
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 1, 0));
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 2, 0));
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 3, 0));
    TEST_ASSERT_EQUAL_INT(3, dedup_count(&buf));
}

void test_expired_entry_not_duplicate(void) {
    dedup_check_and_add(&buf, 42, 0);
    /* 61 seconds later, entry should be expired */
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 42, 61000));
}

void test_purge_removes_old(void) {
    dedup_check_and_add(&buf, 1, 0);
    dedup_check_and_add(&buf, 2, 1000);
    dedup_check_and_add(&buf, 3, 50000);
    /* At t=61000, entries at t=0 and t=1000 are expired */
    dedup_purge(&buf, 61000);
    TEST_ASSERT_EQUAL_INT(1, dedup_count(&buf));
}

void test_full_buffer_evicts_oldest(void) {
    /* Fill buffer */
    for (uint32_t i = 0; i < DEDUP_MAX_ENTRIES; i++) {
        dedup_check_and_add(&buf, i + 1, i * 100);
    }
    TEST_ASSERT_EQUAL_INT(DEDUP_MAX_ENTRIES, dedup_count(&buf));

    /* Add one more — should evict oldest (id=1 at t=0) */
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 9999, 50000));
    TEST_ASSERT_EQUAL_INT(DEDUP_MAX_ENTRIES, dedup_count(&buf));

    /* Original id=1 should no longer be found */
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 1, 50001));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_count_zero);
    RUN_TEST(test_first_packet_not_duplicate);
    RUN_TEST(test_same_packet_is_duplicate);
    RUN_TEST(test_different_packets_not_duplicates);
    RUN_TEST(test_expired_entry_not_duplicate);
    RUN_TEST(test_purge_removes_old);
    RUN_TEST(test_full_buffer_evicts_oldest);
    return UNITY_END();
}
