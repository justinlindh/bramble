#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"

void setUp(void) {}
void tearDown(void) {}

/* DES-9: no surviving flag may collide with FLAG_ENCRYPT. */
void test_no_flag_collides_with_encrypt(void) {
    TEST_ASSERT_NOT_EQUAL(FLAG_ENCRYPT, FLAG_EMERGENCY);
    TEST_ASSERT_NOT_EQUAL(FLAG_ENCRYPT, FLAG_ACK_REQ);
    TEST_ASSERT_NOT_EQUAL(FLAG_ENCRYPT, FLAG_RECEIPT);
    TEST_ASSERT_NOT_EQUAL(FLAG_ENCRYPT, FLAG_CHANNEL);
}

void test_emergency_on_freed_bit6(void) {
    TEST_ASSERT_EQUAL_HEX8(0x40, FLAG_EMERGENCY);
}

/* All flag bits are pairwise disjoint. */
void test_flags_pairwise_disjoint(void) {
    uint8_t all = FLAG_RESERVED_HIGH | FLAG_EMERGENCY | FLAG_ACK_REQ | FLAG_RECEIPT |
                  FLAG_CHANNEL | FLAG_ENCRYPT | FLAG_FRAG_MASK;
    TEST_ASSERT_EQUAL_HEX8(0xFF, all);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_flag_collides_with_encrypt);
    RUN_TEST(test_emergency_on_freed_bit6);
    RUN_TEST(test_flags_pairwise_disjoint);
    return UNITY_END();
}
