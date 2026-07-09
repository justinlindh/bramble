#include "unity.h"
#include "msg_store.h"
#include <stdio.h>
#include <string.h>

void setUp(void) { msg_store_init(); }
void tearDown(void) {}

void test_msg_store_default_channel_index_is_minus_one(void) {
    msg_store_add_ex(0x1234, MSG_DIR_OUTGOING, "hi", 2, 0, 0, 1, MSG_STATUS_SENT);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(-1, m->channel_index);
}

void test_msg_store_add_ex2_persists_channel_index(void) {
    msg_store_add_ex2(0x5678, MSG_DIR_INCOMING, "hey", 3, -80, 7, 2, MSG_STATUS_NONE, 3);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(3, m->channel_index);
}

void test_total_incoming_is_monotonic_and_ignores_outgoing(void) {
    msg_store_init();
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_total_incoming());

    msg_store_add(0x11111111, MSG_DIR_INCOMING, "hi", 2, -70, 5);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_total_incoming());

    msg_store_add(0x22222222, MSG_DIR_OUTGOING, "yo", 2, 0, 0);
    msg_store_add(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, "b", 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_total_incoming());

    msg_store_add(0x33333333, MSG_DIR_BROADCAST_IN, "bc", 2, -80, 3);
    TEST_ASSERT_EQUAL_UINT32(2, msg_store_total_incoming());

    /* Keep counting past the 20-slot ring capacity (the whole point). */
    for (int i = 0; i < 25; i++) {
        msg_store_add(0x44444444, MSG_DIR_INCOMING, "x", 1, -80, 3);
    }
    TEST_ASSERT_EQUAL_UINT32(27, msg_store_total_incoming());
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
}

void test_ring_keeps_newest_window_at_capacity(void) {
    msg_store_init();
    char text[16];
    for (int i = 0; i < MSG_STORE_MAX + 5; i++) {
        snprintf(text, sizeof(text), "m%d", i);
        msg_store_add(0x1111, MSG_DIR_INCOMING, text, strlen(text), -70, 5);
    }
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
    /* Oldest retained message is number 5; newest is MSG_STORE_MAX + 4 */
    char expect[16];
    snprintf(expect, sizeof(expect), "m%d", 5);
    TEST_ASSERT_EQUAL_STRING(expect, msg_store_get(0)->text);
    snprintf(expect, sizeof(expect), "m%d", MSG_STORE_MAX + 4);
    TEST_ASSERT_EQUAL_STRING(expect, msg_store_get(msg_store_count() - 1)->text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_msg_store_default_channel_index_is_minus_one);
    RUN_TEST(test_msg_store_add_ex2_persists_channel_index);
    RUN_TEST(test_total_incoming_is_monotonic_and_ignores_outgoing);
    RUN_TEST(test_ring_keeps_newest_window_at_capacity);
    return UNITY_END();
}
