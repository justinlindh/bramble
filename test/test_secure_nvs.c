#include "unity.h"
#include "secure_nvs.h"

void setUp(void) {}
void tearDown(void) {}

void test_plain_when_encryption_disabled(void) {
    TEST_ASSERT_EQUAL_INT(NVS_INIT_PLAIN, nvs_init_plan(false, false, false, false));
    TEST_ASSERT_EQUAL_INT(NVS_INIT_PLAIN, nvs_init_plan(false, true, true, true));
}

void test_fail_when_keys_partition_missing(void) {
    TEST_ASSERT_EQUAL_INT(NVS_INIT_FAIL, nvs_init_plan(true, false, false, false));
}

void test_fail_when_keys_cfg_unreadable(void) {
    /* Keys partition exists but reading/generating its config failed (e.g.
     * corrupt keys partition or a flash read error). This must NOT erase
     * the main NVS partition: a transient or persistent keys-layer failure
     * would otherwise wipe the device every boot. Fail closed and abort
     * instead, matching the ESP-IDF reference behavior. */
    TEST_ASSERT_EQUAL_INT(NVS_INIT_FAIL, nvs_init_plan(true, true, false, false));
}

void test_secure_when_init_ok(void) {
    TEST_ASSERT_EQUAL_INT(NVS_INIT_SECURE, nvs_init_plan(true, true, true, true));
}

void test_secure_erase_on_migration(void) {
    /* Keys are valid (cfg ok), but nvs_flash_secure_init itself failed to
     * decrypt the main partition: this is the genuine plaintext-to-encrypted
     * migration. Erase and retry; identity and channels regenerate on next
     * load. */
    TEST_ASSERT_EQUAL_INT(NVS_INIT_SECURE_ERASE, nvs_init_plan(true, true, true, false));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_when_encryption_disabled);
    RUN_TEST(test_fail_when_keys_partition_missing);
    RUN_TEST(test_fail_when_keys_cfg_unreadable);
    RUN_TEST(test_secure_when_init_ok);
    RUN_TEST(test_secure_erase_on_migration);
    return UNITY_END();
}
