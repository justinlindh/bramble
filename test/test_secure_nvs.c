#include "unity.h"
#include "secure_nvs.h"

void setUp(void) {}
void tearDown(void) {}

void test_plain_when_encryption_disabled(void) {
    TEST_ASSERT_EQUAL_INT(NVS_INIT_PLAIN, nvs_init_plan(false, false, false));
    TEST_ASSERT_EQUAL_INT(NVS_INIT_PLAIN, nvs_init_plan(false, true, true));
}

void test_fail_when_keys_partition_missing(void) {
    TEST_ASSERT_EQUAL_INT(NVS_INIT_FAIL, nvs_init_plan(true, false, false));
}

void test_secure_when_init_ok(void) {
    TEST_ASSERT_EQUAL_INT(NVS_INIT_SECURE, nvs_init_plan(true, true, true));
}

void test_secure_erase_on_migration(void) {
    /* Encryption on, keys present, but the first secure init failed: this is
     * the plaintext-to-encrypted migration. Erase and retry; identity and
     * channels regenerate on next load. */
    TEST_ASSERT_EQUAL_INT(NVS_INIT_SECURE_ERASE, nvs_init_plan(true, true, false));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_when_encryption_disabled);
    RUN_TEST(test_fail_when_keys_partition_missing);
    RUN_TEST(test_secure_when_init_ok);
    RUN_TEST(test_secure_erase_on_migration);
    return UNITY_END();
}
