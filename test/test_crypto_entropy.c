#include "unity.h"
#include "crypto_entropy.h"
#include <string.h>

static uint32_t g_counter;
static uint32_t stub_source(void) { return ++g_counter; }

void setUp(void) {
    crypto_entropy_set_ready(false);
    g_counter = 0;
}
void tearDown(void) {}

void test_gate_blocks_until_set_ready(void) { TEST_ASSERT_FALSE(crypto_entropy_is_ready()); }

void test_gate_opens_when_set_ready(void) {
    crypto_entropy_set_ready(true);
    TEST_ASSERT_TRUE(crypto_entropy_is_ready());
}

void test_gate_can_be_cleared_again(void) {
    crypto_entropy_set_ready(true);
    crypto_entropy_set_ready(false);
    TEST_ASSERT_FALSE(crypto_entropy_is_ready());
}

void test_fill_fails_closed_and_zeroes_when_gate_shut(void) {
    /* The guard's whole point: a shut gate never leaves predictable key bytes,
     * even for a caller that ignores the return value. */
    uint8_t buf[32];
    memset(buf, 0xAB, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, crypto_entropy_fill(buf, sizeof(buf), stub_source));
    for (size_t i = 0; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, g_counter); /* source never drawn while shut */
}

void test_fill_succeeds_and_draws_source_when_gate_open(void) {
    uint8_t buf[8] = {0};
    uint8_t expect[8];
    uint32_t a = 1, b = 2; /* two 32-bit draws from the stub: 1 then 2 */
    crypto_entropy_set_ready(true);
    TEST_ASSERT_EQUAL_INT(0, crypto_entropy_fill(buf, sizeof(buf), stub_source));
    memcpy(expect, &a, 4);
    memcpy(expect + 4, &b, 4);
    TEST_ASSERT_EQUAL_MEMORY(expect, buf, sizeof(buf));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gate_blocks_until_set_ready);
    RUN_TEST(test_gate_opens_when_set_ready);
    RUN_TEST(test_gate_can_be_cleared_again);
    RUN_TEST(test_fill_fails_closed_and_zeroes_when_gate_shut);
    RUN_TEST(test_fill_succeeds_and_draws_source_when_gate_open);
    return UNITY_END();
}
