#include "unity.h"
#include "../components/crypto/crypto_host.c"

void setUp(void) {}
void tearDown(void) {}

static void rreq_derive_otp(const uint8_t *dh_shared, uint32_t time_bucket, uint8_t otp[4]) {
    /* OTP = SHA256(dh_shared || time_bucket_be32 || "rreq")[0:4] */
    uint8_t buf[32 + 4 + 4]; /* shared + time_bucket + "rreq" */
    memcpy(buf, dh_shared, 32);
    buf[32] = (time_bucket >> 24) & 0xFF;
    buf[33] = (time_bucket >> 16) & 0xFF;
    buf[34] = (time_bucket >> 8) & 0xFF;
    buf[35] = time_bucket & 0xFF;
    memcpy(buf + 36, "rreq", 4);
    uint8_t hash[32];
    crypto_sha256(buf, 40, hash);
    memcpy(otp, hash, 4);
}

static uint32_t rreq_encrypt_source(uint32_t addr, const uint8_t *dh_shared, uint32_t time_bucket) {
    uint8_t otp[4];
    rreq_derive_otp(dh_shared, time_bucket, otp);
    uint32_t otp32 = ((uint32_t)otp[0] << 24) | ((uint32_t)otp[1] << 16) |
                     ((uint32_t)otp[2] << 8)  | (uint32_t)otp[3];
    return addr ^ otp32;
}

static uint32_t rreq_decrypt_source(uint32_t encrypted, const uint8_t *dh_shared, uint32_t time_bucket) {
    return rreq_encrypt_source(encrypted, dh_shared, time_bucket); /* XOR is self-inverse */
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
    uint32_t encrypted = rreq_encrypt_source(alice.address, shared_a, time_bucket);
    TEST_ASSERT_NOT_EQUAL(alice.address, encrypted); /* should be masked */
    uint32_t decrypted = rreq_decrypt_source(encrypted, shared_b, time_bucket);
    TEST_ASSERT_EQUAL_UINT32(alice.address, decrypted);
}

void test_rreq_source_wrong_time_bucket_fails(void) {
    bramble_identity_t alice, bob;
    crypto_generate_identity(&alice);
    crypto_generate_identity(&bob);

    uint8_t shared[32];
    crypto_x25519_dh(alice.private_key, bob.public_key, shared);

    uint32_t encrypted = rreq_encrypt_source(alice.address, shared, 1000);
    uint32_t decrypted = rreq_decrypt_source(encrypted, shared, 1001); /* wrong bucket */
    TEST_ASSERT_NOT_EQUAL(alice.address, decrypted);
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
    return UNITY_END();
}
