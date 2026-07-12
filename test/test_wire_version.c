#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "../components/packet/packet.c"

void setUp(void) {}
void tearDown(void) {}

/*
 * DM forward-secrecy: wire v5 flag day. DM/LOCATION session payloads now
 * carry a 3-byte cleartext ratchet header (epoch || msg_index, authenticated
 * via the AEAD AAD) and are keyed by a per-message ratchet; this is an
 * on-wire layout change v4 packets never had, so it is a hard cutover, same
 * as the v3->v4 bump: bramble_header_is_supported_version's == gate means a
 * v4 packet is no longer just "old", it is REJECTED outright.
 */
void test_current_version_is_five(void) { TEST_ASSERT_EQUAL_UINT8(5, BRAMBLE_VERSION); }

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

void test_supported_version_rejects_v3(void) {
    bramble_header_t h = {.version = 3, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

/*
 * The prior flag-day assertion: a v3 packet was accepted as v4 before this
 * bump. v4 packets are now in the identical position: valid before this
 * bump, REJECTED after it, with no compatibility window where v4 and v5
 * nodes interoperate (the ratchet is mandatory, no downgrade surface).
 */
void test_supported_version_rejects_v4(void) {
    bramble_header_t h = {.version = 4, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_current_version_is_five);
    RUN_TEST(test_supported_version_accepts_current);
    RUN_TEST(test_supported_version_rejects_v1);
    RUN_TEST(test_supported_version_rejects_v2);
    RUN_TEST(test_supported_version_rejects_v3);
    RUN_TEST(test_supported_version_rejects_v4);
    return UNITY_END();
}
