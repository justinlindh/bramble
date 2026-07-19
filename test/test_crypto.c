#include "unity.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

void test_derive_address_deterministic(void) {
    uint8_t pk[32];
    memset(pk, 0xAB, 32);
    uint32_t a1 = crypto_derive_address(pk);
    uint32_t a2 = crypto_derive_address(pk);
    TEST_ASSERT_EQUAL_UINT32(a1, a2);
}

void test_derive_address_different_keys(void) {
    uint8_t pk1[32], pk2[32];
    memset(pk1, 0xAA, 32);
    memset(pk2, 0xBB, 32);
    TEST_ASSERT_NOT_EQUAL(crypto_derive_address(pk1), crypto_derive_address(pk2));
}

void test_aes256gcm_roundtrip(void) {
    uint8_t key[32], nonce[12], tag[16];
    memset(key, 0x01, 32);
    memset(nonce, 0x02, 12);
    const uint8_t pt[] = "Hello Bramble!";
    uint8_t ct[sizeof(pt)], dec[sizeof(pt)];
    TEST_ASSERT_EQUAL(0, crypto_aes256gcm_encrypt(key, nonce, pt, sizeof(pt), NULL, 0, ct, tag));
    TEST_ASSERT_EQUAL(0, crypto_aes256gcm_decrypt(key, nonce, ct, sizeof(pt), NULL, 0, tag, dec));
    TEST_ASSERT_EQUAL_MEMORY(pt, dec, sizeof(pt));
}

void test_aes256gcm_tamper_detected(void) {
    uint8_t key[32], nonce[12], tag[16];
    memset(key, 0x01, 32);
    memset(nonce, 0x02, 12);
    const uint8_t pt[] = "Secret";
    uint8_t ct[sizeof(pt)], dec[sizeof(pt)];
    crypto_aes256gcm_encrypt(key, nonce, pt, sizeof(pt), NULL, 0, ct, tag);
    ct[0] ^= 0xFF; /* tamper */
    TEST_ASSERT_NOT_EQUAL(0,
                          crypto_aes256gcm_decrypt(key, nonce, ct, sizeof(pt), NULL, 0, tag, dec));
}

void test_hkdf_sha256_derives_key(void) {
    uint8_t salt[] = "salt";
    uint8_t ikm[] = "input key material";
    uint8_t info[] = "info";
    uint8_t okm1[32], okm2[32];
    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256(salt, 4, ikm, 18, info, 4, okm1, 32));
    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256(salt, 4, ikm, 18, info, 4, okm2, 32));
    TEST_ASSERT_EQUAL_MEMORY(okm1, okm2, 32);
    /* Different info should give different key */
    uint8_t info2[] = "other";
    uint8_t okm3[32];
    crypto_hkdf_sha256(salt, 4, ikm, 18, info2, 5, okm3, 32);
    TEST_ASSERT_FALSE(memcmp(okm1, okm3, 32) == 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_derive_address_deterministic);
    RUN_TEST(test_derive_address_different_keys);
    RUN_TEST(test_aes256gcm_roundtrip);
    RUN_TEST(test_aes256gcm_tamper_detected);
    RUN_TEST(test_hkdf_sha256_derives_key);
    return UNITY_END();
}
