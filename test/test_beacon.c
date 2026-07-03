#include "unity.h"
#include "network_key.h"
#include "../components/routing/beacon.c"
#include "../components/packet/packet.c"
#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"

void setUp(void) { network_key_clear(); }
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

/*
 * SEC-H2 (Task 3.4, STAGED, not closed: see network_key.h). Mirrors
 * mesh_task.c's provisioned beacon key derivation exactly: HKDF over
 * network_key_get()'s output with label "bramble-beacon-v2", producing
 * the labeled subkey fed into beacon_compute_hmac/verify_hmac as
 * s_beacon_key. beacon_verify_hmac's constant-time-ness is a code-review
 * property (an OR-accumulate compare with no early exit, replacing the
 * old memcmp), not something a functional unit test can observe via
 * timing; this suite verifies it functionally (accept the right key,
 * reject the wrong one) alongside the source change itself.
 */
static void derive_beacon_key_from_network_key(const uint8_t net_key[32], uint8_t beacon_key[32]) {
    const char *salt = "bramble-beacon-v2";
    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256((const uint8_t *)salt, strlen(salt), net_key, 32, NULL,
                                            0, beacon_key, 32));
}

void test_beacon_hmac_under_provisioned_network_key_verifies(void) {
    uint8_t net_key[32];
    crypto_random(net_key, 32);
    network_key_set_provisioned(net_key);

    uint8_t net_key_out[32];
    TEST_ASSERT_EQUAL(0, network_key_get(net_key_out));
    uint8_t beacon_key[32];
    derive_beacon_key_from_network_key(net_key_out, beacon_key);

    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, beacon_key, sizeof(beacon_key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, beacon_key, sizeof(beacon_key)));
}

/* Genuinely discriminating: proves the two network keys derive DIFFERENT
 * beacon subkeys (else the reject assertion below would pass vacuously
 * for the trivial reason that the "different" key wasn't actually
 * different), then confirms verification under the wrong subkey rejects. */
void test_beacon_hmac_rejects_different_network_key(void) {
    uint8_t net_key_a[32];
    uint8_t net_key_b[32];
    crypto_random(net_key_a, 32);
    crypto_random(net_key_b, 32);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(net_key_a, net_key_b, sizeof(net_key_a)));

    uint8_t beacon_key_a[32];
    uint8_t beacon_key_b[32];
    derive_beacon_key_from_network_key(net_key_a, beacon_key_a);
    derive_beacon_key_from_network_key(net_key_b, beacon_key_b);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(beacon_key_a, beacon_key_b, sizeof(beacon_key_a)));

    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, beacon_key_a, sizeof(beacon_key_a));
    TEST_ASSERT_FALSE(beacon_verify_hmac(&b, beacon_key_b, sizeof(beacon_key_b)));
}

/* Fix 4 (red-team panel): beacon_compute_hmac previously covered only the
 * fixed 32 bytes before auth_hmac; the optional name (serialized AFTER
 * auth_hmac, at BEACON_SIZE) was outside the HMAC entirely, so an attacker
 * could rewrite any captured beacon's name and it would still verify,
 * spoofing peer names in the neighbor table/UI even under a provisioned
 * key. This proves the name is now covered: tampering it must break
 * verify. */
void test_beacon_hmac_covers_name_tamper_rejected(void) {
    uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    b.name_len = 5;
    memcpy(b.name, "alice", 5);
    b.name[5] = '\0';
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key))); /* untampered: verifies */

    bramble_beacon_t tampered = b;
    memcpy(tampered.name, "mallo", 5); /* same length, different content */
    TEST_ASSERT_FALSE(beacon_verify_hmac(&tampered, key, sizeof(key)));
}

/* A beacon with no name at all (name_len == 0, the common case) must still
 * compute and verify correctly: the fix must not require a name to be
 * present. */
void test_beacon_hmac_still_verifies_with_no_name(void) {
    uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    TEST_ASSERT_EQUAL(0, b.name_len);
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_beacon_build);
    RUN_TEST(test_beacon_hmac_compute_verify);
    RUN_TEST(test_beacon_hmac_wrong_key);
    RUN_TEST(test_beacon_hmac_zero_key);
    RUN_TEST(test_beacon_hmac_under_provisioned_network_key_verifies);
    RUN_TEST(test_beacon_hmac_rejects_different_network_key);
    RUN_TEST(test_beacon_hmac_covers_name_tamper_rejected);
    RUN_TEST(test_beacon_hmac_still_verifies_with_no_name);
    return UNITY_END();
}
