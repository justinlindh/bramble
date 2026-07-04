#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "../components/packet/packet.c"

void setUp(void) {}
void tearDown(void) {}

/*
 * Phase 1 delivery-core: wire v4 flag day. DATA/LOCATION now carry a
 * relay-mutated prev_hop field (packet.h, BRAMBLE_DATA_PREV_HOP_OFFSET) so
 * reverse-route learning works; this needed an on-wire layout change v3
 * packets never had, so this is a hard cutover, same as the v2->v3 bump:
 * bramble_header_is_supported_version's == gate means a v3 packet is no
 * longer just "old", it is REJECTED outright.
 */
void test_current_version_is_four(void) { TEST_ASSERT_EQUAL_UINT8(4, BRAMBLE_VERSION); }

void test_supported_version_accepts_current(void) {
    bramble_header_t h = {.version = BRAMBLE_VERSION, .type = PKT_TYPE_DATA};
    TEST_ASSERT_TRUE(bramble_header_is_supported_version(&h));
}

void test_supported_version_rejects_v1(void) {
    bramble_header_t h = {.version = 1, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

void test_supported_version_rejects_v2(void) {
    bramble_header_t h = {.version = 2, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

/*
 * The prior flag-day assertion: a pre-freshness v2 packet used to be
 * accepted as v3, then rejected once ws 1.3b landed. v3 packets are now in
 * the identical position v2 was: valid before this bump, REJECTED after
 * it, with no compatibility window where v3 and v4 nodes interoperate.
 */
void test_supported_version_rejects_v3(void) {
    bramble_header_t h = {.version = 3, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_current_version_is_four);
    RUN_TEST(test_supported_version_accepts_current);
    RUN_TEST(test_supported_version_rejects_v1);
    RUN_TEST(test_supported_version_rejects_v2);
    RUN_TEST(test_supported_version_rejects_v3);
    return UNITY_END();
}
