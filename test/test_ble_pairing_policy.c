#include "unity.h"
#include "ble_pairing_policy.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Policy table from the spec: display cb wins; static only without display;
 * neither = Just Works bootstrap. */
void test_mode_display_cb_wins_over_static(void) {
    TEST_ASSERT_EQUAL(BLE_PAIRING_DISPLAY_PASSKEY, ble_pairing_mode_resolve(true, true));
    TEST_ASSERT_EQUAL(BLE_PAIRING_DISPLAY_PASSKEY, ble_pairing_mode_resolve(true, false));
}

void test_mode_static_without_display(void) {
    TEST_ASSERT_EQUAL(BLE_PAIRING_STATIC_PASSKEY, ble_pairing_mode_resolve(false, true));
}

void test_mode_just_works_bootstrap(void) {
    TEST_ASSERT_EQUAL(BLE_PAIRING_JUST_WORKS, ble_pairing_mode_resolve(false, false));
}

void test_passkey_valid_exactly_six_digits(void) {
    TEST_ASSERT_TRUE(ble_pairing_passkey_valid("123456"));
    TEST_ASSERT_TRUE(ble_pairing_passkey_valid("000000"));
    TEST_ASSERT_TRUE(ble_pairing_passkey_valid("999999"));
    TEST_ASSERT_FALSE(ble_pairing_passkey_valid("12345"));
    TEST_ASSERT_FALSE(ble_pairing_passkey_valid("1234567"));
    TEST_ASSERT_FALSE(ble_pairing_passkey_valid("12345a"));
    TEST_ASSERT_FALSE(ble_pairing_passkey_valid(" 12345"));
    TEST_ASSERT_FALSE(ble_pairing_passkey_valid(""));
    TEST_ASSERT_FALSE(ble_pairing_passkey_valid(NULL));
}

void test_passkey_parse_preserves_leading_zeros(void) {
    uint32_t v = 0xFFFFFFFFu;
    TEST_ASSERT_TRUE(ble_pairing_passkey_parse("000042", &v));
    TEST_ASSERT_EQUAL_UINT32(42u, v);
    TEST_ASSERT_TRUE(ble_pairing_passkey_parse("999999", &v));
    TEST_ASSERT_EQUAL_UINT32(999999u, v);
    TEST_ASSERT_FALSE(ble_pairing_passkey_parse("99999x", &v));
    TEST_ASSERT_FALSE(ble_pairing_passkey_parse(NULL, &v));
    TEST_ASSERT_FALSE(ble_pairing_passkey_parse("123456", NULL));
}

/* Exponential backoff on failed pairing attempts (static passkey bit-leak
 * mitigation): 0 failures = no delay, then 1s doubling to a 60s cap. */
void test_backoff_schedule(void) {
    TEST_ASSERT_EQUAL_UINT32(0u, ble_pairing_backoff_ms(0));
    TEST_ASSERT_EQUAL_UINT32(1000u, ble_pairing_backoff_ms(1));
    TEST_ASSERT_EQUAL_UINT32(2000u, ble_pairing_backoff_ms(2));
    TEST_ASSERT_EQUAL_UINT32(4000u, ble_pairing_backoff_ms(3));
    TEST_ASSERT_EQUAL_UINT32(32000u, ble_pairing_backoff_ms(6));
    TEST_ASSERT_EQUAL_UINT32(60000u, ble_pairing_backoff_ms(7));
    TEST_ASSERT_EQUAL_UINT32(60000u, ble_pairing_backoff_ms(100));
}

void test_mode_names_match_rpc_contract(void) {
    TEST_ASSERT_EQUAL_STRING("just-works", ble_pairing_mode_name(BLE_PAIRING_JUST_WORKS));
    TEST_ASSERT_EQUAL_STRING("static-passkey", ble_pairing_mode_name(BLE_PAIRING_STATIC_PASSKEY));
    TEST_ASSERT_EQUAL_STRING("passkey-display", ble_pairing_mode_name(BLE_PAIRING_DISPLAY_PASSKEY));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mode_display_cb_wins_over_static);
    RUN_TEST(test_mode_static_without_display);
    RUN_TEST(test_mode_just_works_bootstrap);
    RUN_TEST(test_passkey_valid_exactly_six_digits);
    RUN_TEST(test_passkey_parse_preserves_leading_zeros);
    RUN_TEST(test_backoff_schedule);
    RUN_TEST(test_mode_names_match_rpc_contract);
    return UNITY_END();
}
