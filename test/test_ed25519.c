/*
 * Ed25519 signature primitive known-answer + malleability tests.
 *
 * Vectors: RFC 8032 Section 7.1 (TEST 1, TEST 2, TEST 3, TEST SHA(abc)).
 * These pin the HOST (OpenSSL EVP_PKEY_ED25519) backend byte-for-byte to the
 * RFC; the DEVICE backend (espressif/libsodium crypto_sign_*) implements the
 * same RFC 8032 construction, so identical vectors pin both backends to the
 * same behavior. The private key format is the libsodium 64-byte layout:
 * seed (32) || public key (32); the host backend consumes only the seed half,
 * so a key generated on either backend signs identically on both.
 *
 * Malleability pin: RFC 8032 requires verifiers to reject S >= L (the group
 * order). Both libsodium (sc25519 canonical check) and OpenSSL enforce this,
 * so the S+L malleated-signature rejection asserted here holds on host AND
 * device. Known divergence deliberately NOT asserted: libsodium additionally
 * rejects small-order public keys / R points, OpenSSL does not. No Bramble
 * code path may treat a signature as valid on one backend and invalid on the
 * other for honest (non-attacker-crafted-key) inputs; the small-order case
 * only differs for attacker-chosen keys and is documented in the phase report.
 */
#include "unity.h"
/* Backend selection: the default build pins the host (OpenSSL) backend; the
 * _nrf_backend build of this same file pins the nRF52840 backend
 * (crypto_esp.c: mbedtls + Monocypher Ed25519) to identical vectors. */
#ifdef BRAMBLE_TEST_NRF_BACKEND
#include "../components/crypto/crypto_esp.c"
#else
#include "../components/crypto/crypto_host.c"
#endif

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

/* Build the 64-byte libsodium-format secret key (seed || pub) from RFC hex. */
static void make_secret_key(const char* seed_hex, const char* pub_hex, uint8_t sk[64],
                            uint8_t pk[32]) {
    hex_to_bytes(seed_hex, sk, 32);
    hex_to_bytes(pub_hex, pk, 32);
    memcpy(sk + 32, pk, 32);
}

/* Run one RFC 8032 known-answer vector: sign must reproduce the exact RFC
 * signature bytes, and verify must accept it. */
static void run_rfc8032_vector(const char* seed_hex, const char* pub_hex, const uint8_t* msg,
                               size_t msg_len, const char* sig_hex) {
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    uint8_t pk[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t expected_sig[BRAMBLE_ED25519_SIG_SIZE];
    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];

    make_secret_key(seed_hex, pub_hex, sk, pk);
    hex_to_bytes(sig_hex, expected_sig, 64);

    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, msg_len, sig));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_sig, sig, 64);
    TEST_ASSERT_TRUE(crypto_ed25519_verify(pk, msg, msg_len, sig));
}

/* -- RFC 8032 Section 7.1 TEST 1: empty message -- */
void test_rfc8032_test1_empty_message(void) {
    run_rfc8032_vector("9d61b19deffd5a60ba844af492ec2cc4"
                       "4449c5697b326919703bac031cae7f60",
                       "d75a980182b10ab7d54bfed3c964073a"
                       "0ee172f3daa62325af021a68f707511a",
                       NULL, 0,
                       "e5564300c360ac729086e2cc806e828a"
                       "84877f1eb8e5d974d873e06522490155"
                       "5fb8821590a33bacc61e39701cf9b46b"
                       "d25bf5f0595bbe24655141438e7a100b");
}

/* -- RFC 8032 Section 7.1 TEST 2: 1-byte message 0x72 -- */
void test_rfc8032_test2_one_byte(void) {
    const uint8_t msg[1] = {0x72};
    run_rfc8032_vector("4ccd089b28ff96da9db6c346ec114e0f"
                       "5b8a319f35aba624da8cf6ed4fb8a6fb",
                       "3d4017c3e843895a92b70aa74d1b7ebc"
                       "9c982ccf2ec4968cc0cd55f12af4660c",
                       msg, sizeof(msg),
                       "92a009a9f0d4cab8720e820b5f642540"
                       "a2b27b5416503f8fb3762223ebdb69da"
                       "085ac1e43e15996e458f3613d0f11d8c"
                       "387b2eaeb4302aeeb00d291612bb0c00");
}

/* -- RFC 8032 Section 7.1 TEST 3: 2-byte message af82 -- */
void test_rfc8032_test3_two_bytes(void) {
    const uint8_t msg[2] = {0xaf, 0x82};
    run_rfc8032_vector("c5aa8df43f9f837bedb7442f31dcb7b1"
                       "66d38535076f094b85ce3a2e0b4458f7",
                       "fc51cd8e6218a1a38da47ed00230f058"
                       "0816ed13ba3303ac5deb911548908025",
                       msg, sizeof(msg),
                       "6291d657deec24024827e69c3abe01a3"
                       "0ce548a284743a445e3680d7db5ac3ac"
                       "18ff9b538d16f290ae67f760984dc659"
                       "4a7c15e9716ed28dc027beceea1ec40a");
}

/* -- RFC 8032 Section 7.1 TEST SHA(abc): 64-byte SHA-512("abc") digest -- */
void test_rfc8032_test_sha_abc(void) {
    uint8_t msg[64];
    hex_to_bytes("ddaf35a193617abacc417349ae204131"
                 "12e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd"
                 "454d4423643ce80e2a9ac94fa54ca49f",
                 msg, 64);
    run_rfc8032_vector("833fe62409237b9d62ec77587520911e"
                       "9a759cec1d19755b7da901b96dca3d42",
                       "ec172b93ad5e563bf4932c70e1245034"
                       "c35467ef2efd4d64ebf819683467e2bf",
                       msg, sizeof(msg),
                       "dc2a4459e7369633a52b1bf277839a00"
                       "201009a3efbf3ecb69bea2186c26b589"
                       "09351fc9ac90b3ecfdfbc7c66431e030"
                       "3dca179c138ac17ad9bef1177331a704");
}

/* -- Malleability: S' = S + L must be REJECTED (RFC 8032 canonical-S rule).
 * A verifier that reduces S mod L would accept the malleated signature; both
 * OpenSSL (host) and libsodium (device) reject it. -- */
void test_malleated_signature_s_plus_l_rejected(void) {
    /* Group order L = 2^252 + 27742317777372353535851937790883648493,
     * little-endian byte encoding (matches the sig's little-endian S). */
    static const uint8_t L_LE[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                                     0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    const uint8_t msg[2] = {0xaf, 0x82};
    uint8_t pk[32];
    uint8_t sig[64];

    hex_to_bytes("fc51cd8e6218a1a38da47ed00230f058"
                 "0816ed13ba3303ac5deb911548908025",
                 pk, 32);
    hex_to_bytes("6291d657deec24024827e69c3abe01a3"
                 "0ce548a284743a445e3680d7db5ac3ac"
                 "18ff9b538d16f290ae67f760984dc659"
                 "4a7c15e9716ed28dc027beceea1ec40a",
                 sig, 64);

    /* Sanity: the untampered signature verifies. */
    TEST_ASSERT_TRUE(crypto_ed25519_verify(pk, msg, sizeof(msg), sig));

    /* Malleate: S (sig[32..63], little-endian) += L. S < L < 2^253, so the
     * sum fits in 256 bits with no carry out of the top byte. */
    unsigned int carry = 0;
    for (int i = 0; i < 32; i++) {
        unsigned int v = (unsigned int)sig[32 + i] + L_LE[i] + carry;
        sig[32 + i] = (uint8_t)(v & 0xff);
        carry = v >> 8;
    }
    TEST_ASSERT_EQUAL(0, carry);

    TEST_ASSERT_FALSE(crypto_ed25519_verify(pk, msg, sizeof(msg), sig));
}

/* -- Tampering: any single flipped signature byte must fail verify -- */
void test_tampered_signature_byte_rejected(void) {
    const uint8_t msg[1] = {0x72};
    uint8_t pk[32];
    uint8_t sig[64];

    hex_to_bytes("3d4017c3e843895a92b70aa74d1b7ebc"
                 "9c982ccf2ec4968cc0cd55f12af4660c",
                 pk, 32);
    hex_to_bytes("92a009a9f0d4cab8720e820b5f642540"
                 "a2b27b5416503f8fb3762223ebdb69da"
                 "085ac1e43e15996e458f3613d0f11d8c"
                 "387b2eaeb4302aeeb00d291612bb0c00",
                 sig, 64);

    TEST_ASSERT_TRUE(crypto_ed25519_verify(pk, msg, sizeof(msg), sig));

    /* Flip one byte in the R half and one in the S half. */
    sig[0] ^= 0x01;
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pk, msg, sizeof(msg), sig));
    sig[0] ^= 0x01;
    sig[63] ^= 0x80;
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pk, msg, sizeof(msg), sig));
}

/* -- Round trip: keypair -> sign -> verify; wrong key and tampered message
 * must fail -- */
void test_keypair_sign_verify_roundtrip(void) {
    uint8_t pk_a[BRAMBLE_ED25519_PUBKEY_SIZE], sk_a[BRAMBLE_ED25519_SECKEY_SIZE];
    uint8_t pk_b[BRAMBLE_ED25519_PUBKEY_SIZE], sk_b[BRAMBLE_ED25519_SECKEY_SIZE];
    uint8_t msg[32];
    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];

    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(pk_a, sk_a));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(pk_b, sk_b));

    /* Distinct keys, and the libsodium sk layout invariant sk = seed || pk
     * (this is what makes keys portable between the two backends). */
    TEST_ASSERT_FALSE(memcmp(pk_a, pk_b, 32) == 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pk_a, sk_a + 32, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pk_b, sk_b + 32, 32);

    /* Public key must not be all zeros (a zeroed entropy-failure artifact). */
    uint8_t zeros[32];
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_FALSE(memcmp(pk_a, zeros, 32) == 0);

    memset(msg, 0x5a, sizeof(msg));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk_a, msg, sizeof(msg), sig));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(pk_a, msg, sizeof(msg), sig));

    /* Wrong public key fails. */
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pk_b, msg, sizeof(msg), sig));

    /* Tampered message fails. */
    msg[7] ^= 0x01;
    TEST_ASSERT_FALSE(crypto_ed25519_verify(pk_a, msg, sizeof(msg), sig));
}

/* -- Deterministic keypair from seed (gosim node identities): RFC 8032
 * TEST 1's seed must expand to TEST 1's public key, sk layout seed||pk,
 * and the expansion must be bit-stable across calls -- */
void test_keypair_from_seed_deterministic_rfc_vector(void) {
    uint8_t seed[32], want_pk[32];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc4"
                 "4449c5697b326919703bac031cae7f60",
                 seed, 32);
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a"
                 "0ee172f3daa62325af021a68f707511a",
                 want_pk, 32);

    uint8_t pk[BRAMBLE_ED25519_PUBKEY_SIZE], sk[BRAMBLE_ED25519_SECKEY_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(seed, pk, sk));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_pk, pk, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(seed, sk, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pk, sk + 32, 32);

    uint8_t pk2[BRAMBLE_ED25519_PUBKEY_SIZE], sk2[BRAMBLE_ED25519_SECKEY_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(seed, pk2, sk2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pk, pk2, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sk, sk2, 64);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rfc8032_test1_empty_message);
    RUN_TEST(test_rfc8032_test2_one_byte);
    RUN_TEST(test_rfc8032_test3_two_bytes);
    RUN_TEST(test_rfc8032_test_sha_abc);
    RUN_TEST(test_malleated_signature_s_plus_l_rejected);
    RUN_TEST(test_tampered_signature_byte_rejected);
    RUN_TEST(test_keypair_sign_verify_roundtrip);
    RUN_TEST(test_keypair_from_seed_deterministic_rfc_vector);
    return UNITY_END();
}
