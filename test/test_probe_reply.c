#include "unity.h"
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_slot_ms_ranges);
    RUN_TEST(test_initial_delay_adds_jitter);
    RUN_TEST(test_attempt_due_spacing);
    RUN_TEST(test_attempts_constant);
    return UNITY_END();
}
