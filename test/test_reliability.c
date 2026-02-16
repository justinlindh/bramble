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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tier_max_retries);
    RUN_TEST(test_pending_ack_add_and_remove);
    RUN_TEST(test_flow_control_window);
    RUN_TEST(test_flow_control_failure_shrinks_window);
    return UNITY_END();
}
