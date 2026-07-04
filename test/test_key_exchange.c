#include "unity.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

void test_double_dh_key_agreement(void) {
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    /* Alice generates ephemeral keypair */
    bramble_identity_t alice_eph;
    crypto_generate_identity(&alice_eph);

    /* Alice computes: ss1 = eph_priv × bob_pub, ss2 = alice_priv × bob_pub */
    uint8_t ss1_a[32], ss2_a[32];
    TEST_ASSERT_EQUAL(0, crypto_x25519_dh(alice_eph.private_key, bob.public_key, ss1_a));
    TEST_ASSERT_EQUAL(0, crypto_x25519_dh(alice.private_key, bob.public_key, ss2_a));

    /* Bob computes: ss1 = bob_priv × eph_pub, ss2 = bob_priv × alice_pub */
    uint8_t ss1_b[32], ss2_b[32];
    TEST_ASSERT_EQUAL(0, crypto_x25519_dh(bob.private_key, alice_eph.public_key, ss1_b));
    TEST_ASSERT_EQUAL(0, crypto_x25519_dh(bob.private_key, alice.public_key, ss2_b));

    /* Shared secrets must match */
    TEST_ASSERT_EQUAL_MEMORY(ss1_a, ss1_b, 32);
    TEST_ASSERT_EQUAL_MEMORY(ss2_a, ss2_b, 32);

    /* Derive session key: HKDF(salt="bramble-dm-v1", ikm=ss1||ss2, info=min_addr||max_addr) */
    uint8_t ikm[64];
    memcpy(ikm, ss1_a, 32);
    memcpy(ikm + 32, ss2_a, 32);

    uint32_t min_addr = alice.address < bob.address ? alice.address : bob.address;
    uint32_t max_addr = alice.address < bob.address ? bob.address : alice.address;
    uint8_t info[8];
    info[0] = (min_addr >> 24) & 0xFF;
    info[1] = (min_addr >> 16) & 0xFF;
    info[2] = (min_addr >> 8) & 0xFF;
    info[3] = min_addr & 0xFF;
    info[4] = (max_addr >> 24) & 0xFF;
    info[5] = (max_addr >> 16) & 0xFF;
    info[6] = (max_addr >> 8) & 0xFF;
    info[7] = max_addr & 0xFF;

    uint8_t session_key_a[32], session_key_b[32];
    const char* salt = "bramble-dm-v1";

    /* Bob constructs same ikm */
    uint8_t ikm_b[64];
    memcpy(ikm_b, ss1_b, 32);
    memcpy(ikm_b + 32, ss2_b, 32);

    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ikm, 64, info, 8,
                                            session_key_a, 32));
    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ikm_b, 64, info, 8,
                                            session_key_b, 32));
    TEST_ASSERT_EQUAL_MEMORY(session_key_a, session_key_b, 32);
}

void test_auth_tag_verification(void) {
    uint8_t key[32];
    memset(key, 0x42, 32);
    uint8_t data[] = "authenticate me";
    uint8_t mac1[32], mac2[32];
    TEST_ASSERT_EQUAL(0, crypto_hmac_sha256(key, 32, data, sizeof(data), mac1));
    TEST_ASSERT_EQUAL(0, crypto_hmac_sha256(key, 32, data, sizeof(data), mac2));
    TEST_ASSERT_EQUAL_MEMORY(mac1, mac2, 32);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_double_dh_key_agreement);
    RUN_TEST(test_auth_tag_verification);
    return UNITY_END();
}
