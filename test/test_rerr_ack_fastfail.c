#include "unity.h"

#include <string.h>

#include "../components/reliability/reliability.c"
#include "../main/rerr_ack_fastfail.c"

static uint32_t s_last_failed_packet_id;
static msg_status_t s_last_failed_status;
static int s_status_update_calls;

bool msg_store_update_status(uint32_t packet_id, msg_status_t status) {
    s_last_failed_packet_id = packet_id;
    s_last_failed_status = status;
    s_status_update_calls++;
    return true;
}

static void setUpState(void) {
    s_last_failed_packet_id = 0;
    s_last_failed_status = MSG_STATUS_NONE;
    s_status_update_calls = 0;
}

void setUp(void) { setUpState(); }

void tearDown(void) {}

void test_rerr_for_dest_fast_fails_matching_pending_ack(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t payload[] = {0xAA, 0xBB};
    int idx = pending_ack_add(&table, 0x12345678, 0xABCDEF01, MSG_TIER_NORMAL, payload,
                              sizeof(payload), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_TRUE(table.entries[idx].active);

    size_t failed = rerr_ack_failfast_for_dest(&table, 0xABCDEF01, "route_broken", NULL, NULL);

    TEST_ASSERT_EQUAL_UINT32(1, failed);
    TEST_ASSERT_FALSE(table.entries[idx].active);
}

void test_rerr_for_dest_does_not_affect_other_destination(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t payload[] = {0x11, 0x22};
    int idx_a = pending_ack_add(&table, 0xAAAA0001, 0xDEAD0001, MSG_TIER_NORMAL, payload,
                                sizeof(payload), 1000);
    int idx_b = pending_ack_add(&table, 0xBBBB0002, 0xDEAD0002, MSG_TIER_NORMAL, payload,
                                sizeof(payload), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx_a);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx_b);

    size_t failed = rerr_ack_failfast_for_dest(&table, 0xDEAD0001, "route_broken", NULL, NULL);

    TEST_ASSERT_EQUAL_UINT32(1, failed);
    TEST_ASSERT_FALSE(table.entries[idx_a].active);
    TEST_ASSERT_TRUE(table.entries[idx_b].active);
}

void test_rerr_fast_fail_updates_msg_store_status_to_failed(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t payload[] = {0x44, 0x55};
    int idx = pending_ack_add(&table, 0xCAFEBABE, 0xFEEDBEEF, MSG_TIER_NORMAL, payload,
                              sizeof(payload), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    (void)rerr_ack_failfast_for_dest(&table, 0xFEEDBEEF, "route_broken", NULL, NULL);

    TEST_ASSERT_EQUAL_INT(1, s_status_update_calls);
    TEST_ASSERT_EQUAL_HEX32(0xCAFEBABE, s_last_failed_packet_id);
    TEST_ASSERT_EQUAL(MSG_STATUS_FAILED, s_last_failed_status);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rerr_for_dest_fast_fails_matching_pending_ack);
    RUN_TEST(test_rerr_for_dest_does_not_affect_other_destination);
    RUN_TEST(test_rerr_fast_fail_updates_msg_store_status_to_failed);
    return UNITY_END();
}
