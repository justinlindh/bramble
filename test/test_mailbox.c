#include "unity.h"
#include "../components/mailbox/mailbox.c"

static mailbox_t mb;

void setUp(void) { mailbox_init(&mb); }
void tearDown(void) {}

void test_mailbox_store_and_retrieve(void) {
    uint8_t p1[] = "hello";
    uint8_t p2[] = "world";
    uint8_t p3[] = "test!";

    TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, 1, 100, p1, 5, 1001, 0));
    TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, 2, 200, p2, 5, 1002, 0));
    TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, 3, 100, p3, 5, 1003, 0));
    TEST_ASSERT_EQUAL_INT(3, mb.count);

    mailbox_entry_t out[8];
    int n = mailbox_retrieve(&mb, 100, out, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(1, mb.count); /* only dest=200 remains */

    n = mailbox_retrieve(&mb, 200, out, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, mb.count);
}

void test_mailbox_per_dest_cap(void) {
    uint8_t p[] = "x";
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, (uint32_t)(i + 1), 100, p, 1, (uint32_t)(i + 1),
                                               (uint32_t)(i * 100)));
    }
    /* Should still be 8 for dest=100 (oldest evicted) */
    TEST_ASSERT_EQUAL_INT(8, mailbox_count_for_dest(&mb, 100));
    TEST_ASSERT_EQUAL_INT(8, mb.count);

    /* Verify oldest (packet_id=1) was evicted: retrieve all and check */
    mailbox_entry_t out[8];
    int n = mailbox_retrieve(&mb, 100, out, 8);
    TEST_ASSERT_EQUAL_INT(8, n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(1, out[i].packet_id); /* id=1 should be gone */
    }
}

void test_mailbox_per_source_cap(void) {
    uint8_t p[] = "x";
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, 42, (uint32_t)(100 + i), p, 1,
                                               (uint32_t)(i + 1), (uint32_t)(i * 100)));
    }
    TEST_ASSERT_EQUAL_INT(8, mailbox_count_for_source(&mb, 42));
    TEST_ASSERT_EQUAL_INT(8, mb.count);
}

void test_mailbox_ttl_expiry(void) {
    uint8_t p[] = "hi";
    mailbox_store(&mb, 1, 100, p, 2, 1, 1000);
    mailbox_store(&mb, 2, 200, p, 2, 2, 2000);
    TEST_ASSERT_EQUAL_INT(2, mb.count);

    /* Advance past TTL */
    mailbox_purge_expired(&mb, 1000 + MAILBOX_TTL_MS);
    TEST_ASSERT_EQUAL_INT(1, mb.count); /* only entry at t=2000 survives */

    mailbox_purge_expired(&mb, 2000 + MAILBOX_TTL_MS);
    TEST_ASSERT_EQUAL_INT(0, mb.count);
}

void test_mailbox_fifo_eviction(void) {
    uint8_t p[] = "z";
    /* Fill all 32 slots with unique dests and sources to avoid per-cap eviction */
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, (uint32_t)(i + 1), (uint32_t)(1000 + i), p, 1,
                                               (uint32_t)(i + 1), (uint32_t)(i * 10)));
    }
    TEST_ASSERT_EQUAL_INT(32, mb.count);

    /* Store one more: should evict oldest (packet_id=1, stored_at=0) */
    TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, 500, 2000, p, 1, 9999, 5000));
    TEST_ASSERT_EQUAL_INT(32, mb.count);

    /* Oldest dest=1000 should be gone */
    TEST_ASSERT_EQUAL_INT(0, mailbox_count_for_dest(&mb, 1000));
    TEST_ASSERT_EQUAL_INT(1, mailbox_count_for_dest(&mb, 2000));
}

void test_mailbox_duplicate_packet_id(void) {
    uint8_t p[] = "dup";
    TEST_ASSERT_EQUAL_INT(0, mailbox_store(&mb, 1, 100, p, 3, 42, 0));
    TEST_ASSERT_EQUAL_INT(-2, mailbox_store(&mb, 2, 200, p, 3, 42, 100));
    TEST_ASSERT_EQUAL_INT(1, mb.count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mailbox_store_and_retrieve);
    RUN_TEST(test_mailbox_per_dest_cap);
    RUN_TEST(test_mailbox_per_source_cap);
    RUN_TEST(test_mailbox_ttl_expiry);
    RUN_TEST(test_mailbox_fifo_eviction);
    RUN_TEST(test_mailbox_duplicate_packet_id);
    return UNITY_END();
}
