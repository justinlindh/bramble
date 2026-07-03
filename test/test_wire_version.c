#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "../components/packet/packet.c"

void setUp(void) {}
void tearDown(void) {}

/*
 * ws 1.3b: wire v3 flag day. Freshness (the 48-bit seq now in all five
 * control-plane MACs) needed an on-wire field that v2 packets never had,
 * so this is a hard cutover: bramble_header_is_supported_version's == gate
 * (packet.c:47) means a v2 packet is no longer just "old", it is REJECTED
 * outright, same as v1 always was.
 */
void test_current_version_is_three(void) {
    TEST_ASSERT_EQUAL_UINT8(3, BRAMBLE_VERSION);
}

void test_supported_version_accepts_current(void) {
    bramble_header_t h = {.version = BRAMBLE_VERSION, .type = PKT_TYPE_DATA};
    TEST_ASSERT_TRUE(bramble_header_is_supported_version(&h));
}

void test_supported_version_rejects_v1(void) {
    bramble_header_t h = {.version = 1, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

/*
 * The flag-day assertion: a pre-freshness v2 packet (the version this
 * exact test asserted ACCEPTS before ws 1.3b) must now be rejected. This
 * is what makes the bump a flag day rather than a soft upgrade: there is
 * no compatibility window where v2 and v3 nodes interoperate.
 */
void test_supported_version_rejects_v2(void) {
    bramble_header_t h = {.version = 2, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_current_version_is_three);
    RUN_TEST(test_supported_version_accepts_current);
    RUN_TEST(test_supported_version_rejects_v1);
    RUN_TEST(test_supported_version_rejects_v2);
    return UNITY_END();
}
