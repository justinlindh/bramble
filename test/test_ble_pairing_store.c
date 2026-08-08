#include "unity.h"
#include "ble_pairing_store.h"
#include "nvs_fake.h"

void setUp(void) { nvs_fake_reset(); }
void tearDown(void) {}

void test_unset_by_default(void) {
    uint32_t v = 123;
    TEST_ASSERT_FALSE(ble_pairing_store_is_set());
    TEST_ASSERT_FALSE(ble_pairing_store_get(&v));
}

void test_set_get_roundtrip(void) {
    TEST_ASSERT_EQUAL_INT(0, ble_pairing_store_set(42));
    uint32_t v = 0;
    TEST_ASSERT_TRUE(ble_pairing_store_is_set());
    TEST_ASSERT_TRUE(ble_pairing_store_get(&v));
    TEST_ASSERT_EQUAL_UINT32(42u, v);
}

void test_clear_removes(void) {
    TEST_ASSERT_EQUAL_INT(0, ble_pairing_store_set(999999));
    TEST_ASSERT_EQUAL_INT(0, ble_pairing_store_clear());
    TEST_ASSERT_FALSE(ble_pairing_store_is_set());
}

void test_clear_when_unset_is_success(void) { TEST_ASSERT_EQUAL_INT(0, ble_pairing_store_clear()); }

void test_nvs_unavailable_fails_closed(void) {
    nvs_fake_set_open_fails(true);
    uint32_t v = 0;
    TEST_ASSERT_FALSE(ble_pairing_store_get(&v));
    TEST_ASSERT_FALSE(ble_pairing_store_is_set());
    TEST_ASSERT_NOT_EQUAL(0, ble_pairing_store_set(1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unset_by_default);
    RUN_TEST(test_set_get_roundtrip);
    RUN_TEST(test_clear_removes);
    RUN_TEST(test_clear_when_unset_is_success);
    RUN_TEST(test_nvs_unavailable_fails_closed);
    return UNITY_END();
}
