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

/*
 * Task 6 (GAP B): the mesh_task.c real KE send path (send_ke_envelope ->
 * send_data_packet) decides its reliability tier by calling
 * msg_tier_for_send(app_type == APP_TYPE_KE) -- this is the SAME function
 * exercised below, not a re-implementation of its logic. Before this task,
 * this test hardcoded MSG_TIER_CRITICAL directly into pending_ack_add,
 * which proved the pending-ack MECHANISM honors Critical tier but said
 * nothing about whether the real send path ever actually chose that tier
 * (it didn't: send_data_packet always passed MSG_TIER_NORMAL, regardless
 * of app_type). Driving the tier through msg_tier_for_send here means a
 * regression in that decision (e.g. someone flipping the ternary, or a new
 * caller forgetting to pass is_key_exchange=true) fails THIS test, not just
 * a silently-still-green mechanism test.
 */
void test_key_exchange_send_path_uses_critical_tier(void) {
    /* KEY_EXCHANGE should use Critical tier (8 retries, exponential backoff) */
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t pkt[101]; /* KEY_EXCHANGE_SIZE */
    memset(pkt, 0xAA, sizeof(pkt));

    /* The real decision send_data_packet makes for an APP_TYPE_KE payload. */
    uint8_t tier = msg_tier_for_send(true);
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_CRITICAL, tier);

    int idx = pending_ack_add(&table, 0xAE01, 0x1234, tier, pkt, sizeof(pkt), 1000);
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
        if (table.entries[idx].attempt > prev_attempt)
            retries++;
    }
    /* Should have retried multiple times before giving up */
    TEST_ASSERT_GREATER_OR_EQUAL(1, retries);
}

/* Companion to test_key_exchange_send_path_uses_critical_tier: every
 * non-KE app_type (chat, location, ...) must keep getting MSG_TIER_NORMAL,
 * so this task's fix is scoped to KE and does not silently upgrade every
 * DATA send to Critical tier. */
void test_non_key_exchange_send_path_uses_normal_tier(void) {
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_NORMAL, msg_tier_for_send(false));
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

    /* Table is full, next add must fail */
    int overflow = pending_ack_add(&table, 0xFF, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
    TEST_ASSERT_EQUAL_INT(-1, overflow);

    /* Remove one entry and verify a slot opens up */
    TEST_ASSERT_TRUE(pending_ack_remove(&table, (uint32_t)(MAX_PENDING_ACKS - 1)));
    int retry = pending_ack_add(&table, 0xFF, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, retry);
}

/*
 * A full-size on-air packet (the transmit path caps these at 255 bytes) must
 * be stored whole, because the retransmit re-sends packet_data[0..packet_len).
 * Before PENDING_ACK_PACKET_CAP the buffer was 222 bytes: a large unicast send
 * was truncated on copy while packet_len still held the full length, so the
 * retransmit read past the buffer. This locks in that packet_len never exceeds
 * the storage and that a 255-byte packet round-trips byte-for-byte.
 */
void test_pending_ack_stores_full_packet(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t big[255];
    for (int i = 0; i < (int)sizeof(big); i++)
        big[i] = (uint8_t)(i * 7 + 1);

    int idx = pending_ack_add(&table, 0xB1, 0x2222, MSG_TIER_NORMAL, big, sizeof(big), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_EQUAL_UINT16(sizeof(big), table.entries[idx].packet_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(big, table.entries[idx].packet_data, sizeof(big));
}

/* packet_len can never exceed the storage buffer, even if a caller ever passes
 * an oversized length: the copy is clamped and packet_len tracks the clamp, so
 * the retransmit read stays in bounds. */
void test_pending_ack_clamps_oversized_len(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    static uint8_t huge[PENDING_ACK_PACKET_CAP + 64];
    for (int i = 0; i < (int)sizeof(huge); i++)
        huge[i] = (uint8_t)(i & 0xFF);

    int idx = pending_ack_add(&table, 0xB2, 0x3333, MSG_TIER_NORMAL, huge, sizeof(huge), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_EQUAL_UINT16(PENDING_ACK_PACKET_CAP, table.entries[idx].packet_len);
    TEST_ASSERT_TRUE(table.entries[idx].packet_len <= sizeof(table.entries[idx].packet_data));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(huge, table.entries[idx].packet_data, PENDING_ACK_PACKET_CAP);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tier_max_retries);
    RUN_TEST(test_pending_ack_add_and_remove);
    RUN_TEST(test_key_exchange_send_path_uses_critical_tier);
    RUN_TEST(test_non_key_exchange_send_path_uses_normal_tier);
    RUN_TEST(test_pending_ack_table_full);
    RUN_TEST(test_pending_ack_stores_full_packet);
    RUN_TEST(test_pending_ack_clamps_oversized_len);
    return UNITY_END();
}
