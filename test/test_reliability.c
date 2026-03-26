#include "unity.h"
#include "../components/reliability/reliability.c"

void setUp(void) {}
void tearDown(void) {}

void test_tier_max_retries(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tier_max_retries(MSG_TIER_BROADCAST));
    TEST_ASSERT_EQUAL_UINT8(3, tier_max_retries(MSG_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT8(8, tier_max_retries(MSG_TIER_CRITICAL));
}

void test_pending_ack_add_and_remove(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);
    uint8_t data[] = {0xAA, 0xBB};
    int idx = pending_ack_add(&table, 42, 0x1234, MSG_TIER_NORMAL, data, 2, 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_TRUE(pending_ack_remove(&table, 42));
    TEST_ASSERT_FALSE(pending_ack_remove(&table, 42));
}

void test_flow_control_window(void) {
    flow_control_t fc;
    flow_init(&fc);
    uint32_t dest = 0xABCD;
    /* Send up to window size (4) */
    for (int i = 0; i < FLOW_WINDOW_SIZE; i++) {
        TEST_ASSERT_TRUE(flow_can_send(&fc, dest));
        flow_on_send(&fc, dest);
    }
    TEST_ASSERT_FALSE(flow_can_send(&fc, dest));
    /* ACK opens a slot */
    flow_on_ack(&fc, dest);
    TEST_ASSERT_TRUE(flow_can_send(&fc, dest));
}

void test_flow_control_failure_shrinks_window(void) {
    flow_control_t fc;
    flow_init(&fc);
    uint32_t dest = 0x5678;
    /* Default window is 4, failure halves to 2 */
    flow_on_failure(&fc, dest);
    /* Should be able to send exactly 2 */
    TEST_ASSERT_TRUE(flow_can_send(&fc, dest));
    flow_on_send(&fc, dest);
    TEST_ASSERT_TRUE(flow_can_send(&fc, dest));
    flow_on_send(&fc, dest);
    TEST_ASSERT_FALSE(flow_can_send(&fc, dest));
}

void test_key_exchange_critical_tier_retries(void) {
    /* KEY_EXCHANGE should use Critical tier (8 retries, exponential backoff) */
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t pkt[101]; /* KEY_EXCHANGE_SIZE */
    memset(pkt, 0xAA, sizeof(pkt));

    /* Add KEY_EXCHANGE as Critical tier */
    int idx = pending_ack_add(&table, 0xAE01, 0x1234, MSG_TIER_CRITICAL, pkt, sizeof(pkt), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_EQUAL_UINT8(8, table.entries[idx].max_attempts);
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_CRITICAL, table.entries[idx].tier);

    /* Verify exponential backoff: tick through retries */
    uint32_t now = 1000;
    int retries = 0;
    for (int step = 0; step < 20 && table.entries[idx].active; step++) {
        now += 5000; /* advance 5s each step */
        uint8_t prev_attempt = table.entries[idx].attempt;
        pending_ack_tick(&table, now);
        if (table.entries[idx].attempt > prev_attempt) retries++;
    }
    /* Should have retried multiple times before giving up */
    TEST_ASSERT_GREATER_OR_EQUAL(1, retries);
}

void test_pending_ack_table_full(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);
    uint8_t data[] = {0x01};

    /* Fill all MAX_PENDING_ACKS slots */
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        int idx = pending_ack_add(&table, (uint32_t)i, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
        TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    }

    /* Table is full — next add must fail */
    int overflow = pending_ack_add(&table, 0xFF, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
    TEST_ASSERT_EQUAL_INT(-1, overflow);

    /* Remove one entry and verify a slot opens up */
    TEST_ASSERT_TRUE(pending_ack_remove(&table, (uint32_t)(MAX_PENDING_ACKS - 1)));
    int retry = pending_ack_add(&table, 0xFF, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, retry);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tier_max_retries);
    RUN_TEST(test_pending_ack_add_and_remove);
    RUN_TEST(test_flow_control_window);
    RUN_TEST(test_flow_control_failure_shrinks_window);
    RUN_TEST(test_key_exchange_critical_tier_retries);
    RUN_TEST(test_pending_ack_table_full);
    return UNITY_END();
}
