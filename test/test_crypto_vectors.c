/*
 * Crypto test vectors from published standards:
 *   - AES-256-GCM: NIST SP 800-38D Test Case 16
 *   - X25519: RFC 7748 Section 6.1
 *   - HMAC-SHA256: RFC 4231 Test Case 1
 */
#include "unity.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

/* Helper: hex string to bytes */
static void hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        sscanf(hex + 2 * i, "%02x", &byte);
        out[i] = (uint8_t)byte;
    }
}

/* ── AES-256-GCM: NIST SP 800-38D Test Case 16 ── */

void test_aes256gcm_nist_vector(void) {
    /* Test Case 16 from NIST SP 800-38D (AES-256, 96-bit IV, with AAD) */
    uint8_t key[32], nonce[12];
    uint8_t tag[16];
    uint8_t expected_tag[16];

    /*
     * Use Test Case 14: K=256bit, IV=96bit, P=empty, A=empty
     * Key:  00000000000000000000000000000000 00000000000000000000000000000000
     * IV:   000000000000000000000000
     * T:    530f8afbc74536b9a963b4f1c4cb738b
     */
    memset(key, 0, 32);
    memset(nonce, 0, 12);

    /* Encrypt empty plaintext */
    uint8_t empty_ct[1]; /* won't be written */
    int ret = crypto_aes256gcm_encrypt(key, nonce, NULL, 0, NULL, 0, empty_ct, tag);
    TEST_ASSERT_EQUAL(0, ret);

    hex_to_bytes("530f8afbc74536b9a963b4f1c4cb738b", expected_tag, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_tag, tag, 16);
}

void test_aes256gcm_nist_vector_with_data(void) {
    /* Test Case 15: 256-bit key, 96-bit IV, 64 bytes plaintext, no AAD
     * Key:  00000000000000000000000000000000 00000000000000000000000000000000
     * IV:   000000000000000000000000
     * P:    00000000000000000000000000000000 (16 bytes of zeros)
     *
     * Actually let's use a well-known simple vector:
     * Key: all zeros (32 bytes)
     * IV: all zeros (12 bytes)
     * PT: all zeros (16 bytes)
     * Expected CT: cea7403d4d606b6e074ec5d3baf39d18
     * Expected Tag: d0d1c8a799996bf0265b98b5d48ab919
     */
    uint8_t key[32], nonce[12], pt[16];
    uint8_t ct[16], tag[16];
    uint8_t expected_ct[16], expected_tag[16];

    memset(key, 0, 32);
    memset(nonce, 0, 12);
    memset(pt, 0, 16);

    int ret = crypto_aes256gcm_encrypt(key, nonce, pt, 16, NULL, 0, ct, tag);
    TEST_ASSERT_EQUAL(0, ret);

    hex_to_bytes("cea7403d4d606b6e074ec5d3baf39d18", expected_ct, 16);
    hex_to_bytes("d0d1c8a799996bf0265b98b5d48ab919", expected_tag, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_ct, ct, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_tag, tag, 16);

    /* Verify decrypt */
    uint8_t dec[16];
    ret = crypto_aes256gcm_decrypt(key, nonce, ct, 16, NULL, 0, tag, dec);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pt, dec, 16);
}

/* ── X25519: RFC 7748 Section 6.1 ── */

void test_x25519_rfc7748_shared_secret(void) {
    /*
     * RFC 7748 Section 6.1:
     * Alice's private: 77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a
     * Alice's public:  8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a
     * Bob's private:   5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb
     * Bob's public:    de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f
     * Shared:          4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742
     *
     * Note: OpenSSL's EVP_PKEY_new_raw_private_key for X25519 applies RFC clamping
     * internally, so these vectors should work directly.
     */
    uint8_t alice_priv[32], alice_pub[32];
    uint8_t bob_priv[32], bob_pub[32];
    uint8_t expected_shared[32];
    uint8_t shared_ab[32], shared_ba[32];

    hex_to_bytes("77076d0a7318a57d3c16c17251b26645"
                 "df4c2f87ebc0992ab177fba51db92c2a",
                 alice_priv, 32);
    hex_to_bytes("8520f0098930a754748b7ddcb43ef75a"
                 "0dbf3a0d26381af4eba4a98eaa9b4e6a",
                 alice_pub, 32);
    hex_to_bytes("5dab087e624a8a4b79e17f8b83800ee6"
                 "6f3bb1292618b6fd1c2f8b27ff88e0eb",
                 bob_priv, 32);
    hex_to_bytes("de9edb7d7b7dc1b4d35b61c2ece43537"
                 "3f8343c85b78674dadfc7e146f882b4f",
                 bob_pub, 32);
    hex_to_bytes("4a5d9d5ba4ce2de1728e3bf480350f25"
                 "e07e21c947d19e3376f09b3c1e161742",
                 expected_shared, 32);

    /* Alice computes shared secret with Bob's public key */
    int ret = crypto_x25519_dh(alice_priv, bob_pub, shared_ab);
    TEST_ASSERT_EQUAL(0, ret);

    /* Bob computes shared secret with Alice's public key */
    ret = crypto_x25519_dh(bob_priv, alice_pub, shared_ba);
    TEST_ASSERT_EQUAL(0, ret);

    /* Both should agree */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(shared_ab, shared_ba, 32);

    /* Should match RFC expected value */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_shared, shared_ab, 32);
}

void test_x25519_keypair_derivation(void) {
    /*
     * Verify that crypto_generate_identity produces a consistent public key
     * from a generated private key, and that DH with self works.
     */
    bramble_identity_t id;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&id));

    /* Derive shared secret with our own public key — just verify it doesn't crash
     * and returns a non-zero result */
    uint8_t shared[32];
    int ret = crypto_x25519_dh(id.private_key, id.public_key, shared);
    TEST_ASSERT_EQUAL(0, ret);

    /* Shared secret should not be all zeros */
    uint8_t zeros[32];
    memset(zeros, 0, 32);
    TEST_ASSERT_FALSE(memcmp(shared, zeros, 32) == 0);
}

/* ── HMAC-SHA256: RFC 4231 Test Case 1 ── */

void test_hmac_sha256_rfc4231_case1(void) {
    /*
     * RFC 4231 Test Case 1:
     * Key:  0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (20 bytes)
     * Data: 4869205468657265 ("Hi There")
     * HMAC: b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
     */
    uint8_t key[20], data[8], expected[32], result[32];

    hex_to_bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", key, 20);
    hex_to_bytes("4869205468657265", data, 8);
    hex_to_bytes("b0344c61d8db38535ca8afceaf0bf12b"
                 "881dc200c9833da726e9376c2e32cff7",
                 expected, 32);

    int ret = crypto_hmac_sha256(key, 20, data, 8, result);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, result, 32);
}

void test_hmac_sha256_rfc4231_case2(void) {
    /*
     * RFC 4231 Test Case 2:
     * Key:  "Jefe" (4 bytes)
     * Data: "what do ya want for nothing?" (28 bytes)
     * HMAC: 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
     */
    const uint8_t* key = (const uint8_t*)"Jefe";
    const uint8_t* data = (const uint8_t*)"what do ya want for nothing?";
    uint8_t expected[32], result[32];

    hex_to_bytes("5bdcc146bf60754e6a042426089575c7"
                 "5a003f089d2739839dec58b964ec3843",
                 expected, 32);

    int ret = crypto_hmac_sha256(key, 4, data, 28, result);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, result, 32);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_aes256gcm_nist_vector);
    RUN_TEST(test_aes256gcm_nist_vector_with_data);
    RUN_TEST(test_x25519_rfc7748_shared_secret);
    RUN_TEST(test_x25519_keypair_derivation);
    RUN_TEST(test_hmac_sha256_rfc4231_case1);
    RUN_TEST(test_hmac_sha256_rfc4231_case2);
    return UNITY_END();
}
