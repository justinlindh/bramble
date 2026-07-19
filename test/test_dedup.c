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

    /* Add one more: should evict oldest (id=1 at t=0) */
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 9999, 50000));
    TEST_ASSERT_EQUAL_INT(DEDUP_MAX_ENTRIES, dedup_count(&buf));

    /* Original id=1 should no longer be found */
    TEST_ASSERT_FALSE(dedup_check_and_add(&buf, 1, 50001));
}

/* Task 6: dedup_contains is a pure, non-inserting peek, used by the
 * duplicate-DATA re-ACK decision (mesh_task.c) to ask "did I already record
 * this key" without polluting the table on a miss. */
void test_contains_false_when_absent(void) {
    TEST_ASSERT_FALSE(dedup_contains(&buf, 1234, 0));
    /* A miss must not have inserted anything. */
    TEST_ASSERT_EQUAL_INT(0, dedup_count(&buf));
}

void test_contains_true_after_add(void) {
    dedup_check_and_add(&buf, 1234, 1000);
    TEST_ASSERT_TRUE(dedup_contains(&buf, 1234, 1500));
}

void test_contains_false_after_expiry(void) {
    dedup_check_and_add(&buf, 1234, 0);
    /* 61 seconds later, the entry is past DEDUP_EXPIRY_MS. */
    TEST_ASSERT_FALSE(dedup_contains(&buf, 1234, 61000));
}

/*
 * The exact mechanism GAP A (Task 6) relies on: handle_data records a
 * (src_addr, packet_id)-qualified key (packet_id ^ src_addr, mirroring
 * s_flood_dedup's collision-safe scheme) into a "recently delivered" dedup
 * table right after sending the ACK for a genuinely NEW unicast delivery.
 * A later duplicate of that same DATA frame (the sender's retransmit after
 * its first ACK was lost) is recognized via dedup_contains and triggers a
 * re-sent ACK -- but the caller (mesh_process_rx_packet) never reaches
 * handle_data's decrypt/deliver logic for a dedup-buffer hit, so the app
 * only ever sees the message once.
 */
void test_delivered_dedup_enables_reack_without_redelivery(void) {
    dedup_buffer_t delivered;
    dedup_init(&delivered);
    uint32_t src_addr = 0xAABBCCDDu;
    uint32_t packet_id = 42;
    uint32_t key = packet_id ^ src_addr;

    /* First arrival: not yet delivered. The app sees it once here, and the
     * caller records the delivery right after sending the first ACK. */
    TEST_ASSERT_FALSE(dedup_contains(&delivered, key, 1000));
    dedup_check_and_add(&delivered, key, 1000);

    /* Duplicate DATA arrives later (sender retransmit after a lost ACK):
     * recognized as already delivered, so the caller re-sends the ACK
     * without re-invoking delivery. */
    TEST_ASSERT_TRUE(dedup_contains(&delivered, key, 5000));

    /* A different packet_id from the same sender (a genuinely new message)
     * must NOT be mistaken for the earlier delivery. */
    TEST_ASSERT_FALSE(dedup_contains(&delivered, (packet_id + 1) ^ src_addr, 5000));
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
    RUN_TEST(test_contains_false_when_absent);
    RUN_TEST(test_contains_true_after_add);
    RUN_TEST(test_contains_false_after_expiry);
    RUN_TEST(test_delivered_dedup_enables_reack_without_redelivery);
    return UNITY_END();
}
