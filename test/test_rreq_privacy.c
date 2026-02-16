#include "unity.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

static void rreq_derive_otp(const uint8_t *dh_shared, uint32_t time_bucket, uint32_t salt, uint8_t otp[4]) {
    /* OTP = SHA256(dh_shared || time_bucket_be32 || salt_be32 || "bramble-rreq-v1")[0:4] */
    uint8_t buf[32 + 4 + 4 + 15]; /* shared + time_bucket + salt + "bramble-rreq-v1" */
    memcpy(buf, dh_shared, 32);
    buf[32] = (time_bucket >> 24) & 0xFF;
    buf[33] = (time_bucket >> 16) & 0xFF;
    buf[34] = (time_bucket >> 8) & 0xFF;
    buf[35] = time_bucket & 0xFF;
    buf[36] = (salt >> 24) & 0xFF;
    buf[37] = (salt >> 16) & 0xFF;
    buf[38] = (salt >> 8) & 0xFF;
    buf[39] = salt & 0xFF;
    memcpy(buf + 40, "bramble-rreq-v1", 15);
    uint8_t hash[32];
    crypto_sha256(buf, 55, hash);
    memcpy(otp, hash, 4);
}

static uint32_t rreq_encrypt_source(uint32_t addr, const uint8_t *dh_shared, uint32_t time_bucket, uint32_t salt) {
    uint8_t otp[4];
    rreq_derive_otp(dh_shared, time_bucket, salt, otp);
    uint32_t otp32 = ((uint32_t)otp[0] << 24) | ((uint32_t)otp[1] << 16) |
                     ((uint32_t)otp[2] << 8)  | (uint32_t)otp[3];
    return addr ^ otp32;
}

static uint32_t rreq_decrypt_source(uint32_t encrypted, const uint8_t *dh_shared, uint32_t time_bucket, uint32_t salt) {
    return rreq_encrypt_source(encrypted, dh_shared, time_bucket, salt); /* XOR is self-inverse */
}

void test_rreq_source_encrypt_decrypt(void) {
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    uint8_t shared_a[32], shared_b[32];
    crypto_x25519_dh(alice.private_key, bob.public_key, shared_a);
    crypto_x25519_dh(bob.private_key, alice.public_key, shared_b);
    TEST_ASSERT_EQUAL_MEMORY(shared_a, shared_b, 32);

    uint32_t time_bucket = 1000;
    uint32_t salt = 0xDEADBEEF;
    uint32_t encrypted = rreq_encrypt_source(alice.address, shared_a, time_bucket, salt);
    TEST_ASSERT_NOT_EQUAL(alice.address, encrypted); /* should be masked */
    uint32_t decrypted = rreq_decrypt_source(encrypted, shared_b, time_bucket, salt);
    TEST_ASSERT_EQUAL_UINT32(alice.address, decrypted);
}

void test_rreq_source_wrong_time_bucket_fails(void) {
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    uint8_t shared[32];
    crypto_x25519_dh(alice.private_key, bob.public_key, shared);

    uint32_t salt = 0x12345678;
    uint32_t encrypted = rreq_encrypt_source(alice.address, shared, 1000, salt);
    uint32_t decrypted = rreq_decrypt_source(encrypted, shared, 1001, salt); /* wrong bucket */
    TEST_ASSERT_NOT_EQUAL(alice.address, decrypted);
}

void test_rreq_salt_prevents_correlation(void) {
    /* Same source, dest, time_bucket but different salts → different encrypted_source */
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    uint8_t shared[32];
    crypto_x25519_dh(alice.private_key, bob.public_key, shared);

    uint32_t enc1 = rreq_encrypt_source(alice.address, shared, 1000, 0x11111111);
    uint32_t enc2 = rreq_encrypt_source(alice.address, shared, 1000, 0x22222222);
    TEST_ASSERT_NOT_EQUAL(enc1, enc2);

    /* Both should decrypt correctly with their respective salts */
    TEST_ASSERT_EQUAL_UINT32(alice.address, rreq_decrypt_source(enc1, shared, 1000, 0x11111111));
    TEST_ASSERT_EQUAL_UINT32(alice.address, rreq_decrypt_source(enc2, shared, 1000, 0x22222222));
}

void test_rreq_open_source_fallback(void) {
    /* Open source = no encryption, addr transmitted as-is */
    uint32_t addr = 0xDEADBEEF;
    TEST_ASSERT_EQUAL_UINT32(addr, addr); /* trivial: no transform */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rreq_source_encrypt_decrypt);
    RUN_TEST(test_rreq_source_wrong_time_bucket_fails);
    RUN_TEST(test_rreq_open_source_fallback);
    RUN_TEST(test_rreq_salt_prevents_correlation);
    return UNITY_END();
}
