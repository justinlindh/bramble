#include "unity.h"
#include "../components/routing/beacon.c"
#include "../components/packet/packet.c"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

void test_beacon_build(void) {
    bramble_beacon_t b = beacon_build(0xAABBCCDD, 0x11223344, 120, 85, 3, 5, 0x01, 1000000, 500);
    TEST_ASSERT_EQUAL(BRAMBLE_VERSION, b.header.version);
    TEST_ASSERT_EQUAL(PKT_TYPE_BEACON, b.header.type);
    TEST_ASSERT_EQUAL(0xFFFFFFFF, b.header.dest_addr);
    TEST_ASSERT_EQUAL(0xAABBCCDD, b.src_addr);
    TEST_ASSERT_EQUAL(0x11223344, b.pubkey_hash);
    TEST_ASSERT_EQUAL(120, b.uptime_min);
    TEST_ASSERT_EQUAL(85, b.battery_pct);
    TEST_ASSERT_EQUAL(3, b.tx_queue_depth);
    TEST_ASSERT_EQUAL(5, b.neighbor_count);
    TEST_ASSERT_EQUAL(0x01, b.flags);
    TEST_ASSERT_EQUAL(1000000, b.network_time);
    TEST_ASSERT_EQUAL(500, b.time_confidence);
}

void test_beacon_hmac_compute_verify(void) {
    uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, key, sizeof(key));
    /* HMAC should be non-zero */
    uint8_t zero[4] = {0};
    TEST_ASSERT_FALSE(memcmp(b.auth_hmac, zero, 4) == 0);
    /* Verify should pass */
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

void test_beacon_hmac_wrong_key(void) {
    uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t bad_key[16] = {99,98,97,96,95,94,93,92,91,90,89,88,87,86,85,84};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_FALSE(beacon_verify_hmac(&b, bad_key, sizeof(bad_key)));
}

void test_beacon_hmac_zero_key(void) {
    uint8_t key[16] = {0};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_beacon_build);
    RUN_TEST(test_beacon_hmac_compute_verify);
    RUN_TEST(test_beacon_hmac_wrong_key);
    RUN_TEST(test_beacon_hmac_zero_key);
    return UNITY_END();
}
