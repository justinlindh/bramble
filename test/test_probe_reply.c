#include "unity.h"
#include <string.h>   /* add near the top with the existing includes */
#include "probe_reply.h"

void setUp(void) {}
void tearDown(void) {}

void test_slot_ms_ranges(void) {
    TEST_ASSERT_EQUAL_UINT32(300u, probe_reply_slot_ms(0));   /* 0 % 6 == 0 */
    TEST_ASSERT_EQUAL_UINT32(410u, probe_reply_slot_ms(1));   /* 1 % 6 == 1 */
    TEST_ASSERT_EQUAL_UINT32(850u, probe_reply_slot_ms(5));   /* 5 % 6 == 5 */
    TEST_ASSERT_EQUAL_UINT32(300u, probe_reply_slot_ms(6));   /* wraps */
}

void test_initial_delay_adds_jitter(void) {
    TEST_ASSERT_EQUAL_UINT32(410u + 119u, probe_reply_initial_delay_ms(1, 119u));
    TEST_ASSERT_EQUAL_UINT32(300u + 0u, probe_reply_initial_delay_ms(0, 0u));
}

void test_attempt_due_spacing(void) {
    uint32_t now = 10000u;
    uint32_t initial = 500u;
    TEST_ASSERT_EQUAL_UINT32(10500u, probe_reply_attempt_due_ms(now, initial, 0));
    TEST_ASSERT_EQUAL_UINT32(10640u, probe_reply_attempt_due_ms(now, initial, 1));
    TEST_ASSERT_EQUAL_UINT32(10780u, probe_reply_attempt_due_ms(now, initial, 2));
}

void test_attempts_constant(void) {
    TEST_ASSERT_EQUAL_INT(3, PROBE_REPLY_ATTEMPTS);
    TEST_ASSERT_EQUAL_INT(140, PROBE_REPLY_RETRY_SPACING_MS);
}

void test_queue_insert_uses_free_slot(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint8_t buf[6] = {1,2,3,4,5,6};
    int slot = probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 6, PROBE_REPLY_ATTEMPTS, 5000u);
    TEST_ASSERT_EQUAL_INT(0, slot);
    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL_UINT8(6, q[0].wire_len);
    TEST_ASSERT_EQUAL_UINT8(PROBE_REPLY_ATTEMPTS, q[0].attempts_total);
    TEST_ASSERT_EQUAL_UINT8(0, q[0].attempts_sent);
    TEST_ASSERT_EQUAL_UINT32(5000u, q[0].due_at_ms);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, q[0].buf, 6);
}

void test_queue_insert_full_returns_negative(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint8_t buf[1] = {0};
    for (int i = 0; i < PROBE_REPLY_QUEUE_CAPACITY; i++)
        TEST_ASSERT_TRUE(probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 100u) >= 0);
    TEST_ASSERT_EQUAL_INT(-1, probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 100u));
}

void test_queue_earliest_due_picks_min(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint32_t due = 12345u;
    TEST_ASSERT_FALSE(probe_reply_queue_earliest_due(q, PROBE_REPLY_QUEUE_CAPACITY, &due));
    uint8_t buf[1] = {0};
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 900u);
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 300u);
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 600u);
    TEST_ASSERT_TRUE(probe_reply_queue_earliest_due(q, PROBE_REPLY_QUEUE_CAPACITY, &due));
    TEST_ASSERT_EQUAL_UINT32(300u, due);
}

void test_queue_find_due_respects_now(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint8_t buf[1] = {0};
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 500u);
    TEST_ASSERT_EQUAL_INT(-1, probe_reply_queue_find_due(q, PROBE_REPLY_QUEUE_CAPACITY, 400u));
    TEST_ASSERT_EQUAL_INT(0,  probe_reply_queue_find_due(q, PROBE_REPLY_QUEUE_CAPACITY, 500u));
    TEST_ASSERT_EQUAL_INT(0,  probe_reply_queue_find_due(q, PROBE_REPLY_QUEUE_CAPACITY, 600u));
}

void test_queue_apply_denied_frees_slot(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint8_t buf[1] = {0};
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 100u);
    probe_reply_queue_apply_result(&q[0], PROBE_REPLY_TX_DENIED, 1000u);
    TEST_ASSERT_FALSE(q[0].used);   /* deny-stop: whole reply abandoned */
}

void test_queue_apply_sent_retry_reschedules(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint8_t buf[1] = {0};
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 100u);
    probe_reply_queue_apply_result(&q[0], PROBE_REPLY_TX_SENT, 1000u);
    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL_UINT8(1, q[0].attempts_sent);
    TEST_ASSERT_EQUAL_UINT32(1000u + PROBE_REPLY_RETRY_SPACING_MS, q[0].due_at_ms);
}

void test_queue_apply_sent_final_frees(void) {
    pending_probe_reply_t q[PROBE_REPLY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    uint8_t buf[1] = {0};
    probe_reply_queue_insert(q, PROBE_REPLY_QUEUE_CAPACITY, buf, 1, 3, 100u);
    q[0].attempts_sent = 2;   /* one send short of attempts_total (3) */
    probe_reply_queue_apply_result(&q[0], PROBE_REPLY_TX_SENT, 1000u);
    TEST_ASSERT_FALSE(q[0].used);   /* third send completes and frees the slot */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_slot_ms_ranges);
    RUN_TEST(test_initial_delay_adds_jitter);
    RUN_TEST(test_attempt_due_spacing);
    RUN_TEST(test_attempts_constant);
    RUN_TEST(test_queue_insert_uses_free_slot);
    RUN_TEST(test_queue_insert_full_returns_negative);
    RUN_TEST(test_queue_earliest_due_picks_min);
    RUN_TEST(test_queue_find_due_respects_now);
    RUN_TEST(test_queue_apply_denied_frees_slot);
    RUN_TEST(test_queue_apply_sent_retry_reschedules);
    RUN_TEST(test_queue_apply_sent_final_frees);
    return UNITY_END();
}
