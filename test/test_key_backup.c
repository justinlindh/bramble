#include "unity.h"
#include "../components/ble/key_backup.c"

void setUp(void) {}
void tearDown(void) {}

void test_init_idle(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    TEST_ASSERT_EQUAL(BACKUP_IDLE, key_backup_get_state(&ctx));
}

void test_request_moves_to_requested(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    TEST_ASSERT_EQUAL(0, key_backup_request(&ctx, 1000));
    TEST_ASSERT_EQUAL(BACKUP_REQUESTED, key_backup_get_state(&ctx));
}

void test_authorize_without_request_fails(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    TEST_ASSERT_EQUAL(-1, key_backup_authorize(&ctx, 1000));
}

void test_request_then_authorize(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    key_backup_request(&ctx, 1000);
    TEST_ASSERT_EQUAL(0, key_backup_authorize(&ctx, 2000));
    TEST_ASSERT_EQUAL(BACKUP_AUTHORIZED, key_backup_get_state(&ctx));
}

void test_request_then_timeout(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    key_backup_request(&ctx, 1000);
    key_backup_tick(&ctx, 1000 + BACKUP_AUTH_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(BACKUP_TIMEOUT, key_backup_get_state(&ctx));
}

void test_export_produces_96_bytes(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    key_backup_request(&ctx, 1000);
    key_backup_authorize(&ctx, 2000);

    uint8_t privkey[32], pubkey[32];
    memset(privkey, 0xAA, 32);
    memset(pubkey, 0xBB, 32);
    uint32_t addr = 0x12345678;
    uint8_t enc_key[32];
    memset(enc_key, 0xCC, 32);

    uint8_t blob[96];
    size_t blob_len = 0;
    int ret = key_backup_export(&ctx, privkey, pubkey, addr, enc_key, blob, sizeof(blob), &blob_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(96, blob_len);
    TEST_ASSERT_EQUAL(BACKUP_SENDING, key_backup_get_state(&ctx));
}

void test_export_import_roundtrip(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    key_backup_request(&ctx, 1000);
    key_backup_authorize(&ctx, 2000);

    uint8_t privkey[32], pubkey[32];
    for (int i = 0; i < 32; i++) { privkey[i] = (uint8_t)i; pubkey[i] = (uint8_t)(i + 32); }
    uint32_t addr = 0xDEADBEEF;
    uint8_t enc_key[32];
    memset(enc_key, 0x42, 32);

    uint8_t blob[96];
    size_t blob_len = 0;
    TEST_ASSERT_EQUAL(0, key_backup_export(&ctx, privkey, pubkey, addr, enc_key, blob, sizeof(blob), &blob_len));

    uint8_t r_priv[32], r_pub[32];
    uint32_t r_addr;
    TEST_ASSERT_EQUAL(0, key_backup_import(blob, blob_len, enc_key, r_priv, r_pub, &r_addr));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(privkey, r_priv, 32);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pubkey, r_pub, 32);
    TEST_ASSERT_EQUAL_UINT32(addr, r_addr);
}

void test_import_wrong_key_fails(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    key_backup_request(&ctx, 1000);
    key_backup_authorize(&ctx, 2000);

    uint8_t privkey[32], pubkey[32];
    memset(privkey, 0x11, 32);
    memset(pubkey, 0x22, 32);
    uint32_t addr = 0xCAFE;
    uint8_t enc_key[32];
    memset(enc_key, 0x33, 32);

    uint8_t blob[96];
    size_t blob_len = 0;
    key_backup_export(&ctx, privkey, pubkey, addr, enc_key, blob, sizeof(blob), &blob_len);

    uint8_t wrong_key[32];
    memset(wrong_key, 0xFF, 32);
    uint8_t r_priv[32], r_pub[32];
    uint32_t r_addr;
    TEST_ASSERT_EQUAL(-1, key_backup_import(blob, blob_len, wrong_key, r_priv, r_pub, &r_addr));
}

void test_double_request_while_busy(void) {
    key_backup_ctx_t ctx;
    key_backup_init(&ctx);
    key_backup_request(&ctx, 1000);
    TEST_ASSERT_EQUAL(-1, key_backup_request(&ctx, 2000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_idle);
    RUN_TEST(test_request_moves_to_requested);
    RUN_TEST(test_authorize_without_request_fails);
    RUN_TEST(test_request_then_authorize);
    RUN_TEST(test_request_then_timeout);
    RUN_TEST(test_export_produces_96_bytes);
    RUN_TEST(test_export_import_roundtrip);
    RUN_TEST(test_import_wrong_key_fails);
    RUN_TEST(test_double_request_while_busy);
    return UNITY_END();
}
