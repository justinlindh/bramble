#include "unity.h"
#include "../main/ct_strcmp.h"
#include "rpc_auth.h"

void setUp(void) {}
void tearDown(void) {}

void test_ct_strcmp_match(void) {
    TEST_ASSERT_EQUAL(0, ct_strcmp("abc", "abc"));
}

void test_ct_strcmp_mismatch(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("abc", "abd"));
}

void test_ct_strcmp_length_mismatch(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("abc", "ab"));
}

void test_ct_strcmp_both_empty(void) {
    TEST_ASSERT_EQUAL(0, ct_strcmp("", ""));
}

void test_ct_strcmp_one_empty_a(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("a", ""));
}

void test_ct_strcmp_one_empty_b(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("", "a"));
}

/* ── Unauthenticated allowlist policy (auth required by default) ────── */

void test_unauth_allows_ping(void) {
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.ping", false));
}

void test_unauth_allows_get_version(void) {
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.getVersion", false));
}

void test_unauth_denies_get_auth_token(void) {
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.getAuthToken", false));
}

void test_unauth_denies_set_auth_token(void) {
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.setAuthToken", false));
}

void test_unauth_denies_ota_update(void) {
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.otaUpdate", false));
}

void test_unauth_denies_get_messages(void) {
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.getMessages", false));
}

void test_unauth_denies_unknown_method(void) {
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.doesNotExist", false));
}

void test_unauth_denies_prefix_lookalike(void) {
    /* Allowlist compares whole strings, not prefixes */
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.getVersionX", false));
    TEST_ASSERT_FALSE(rpc_auth_method_allowed("bramble.pingFlood", false));
}

void test_unauth_denies_null_method(void) {
    TEST_ASSERT_FALSE(rpc_auth_method_allowed(NULL, false));
    TEST_ASSERT_FALSE(rpc_auth_method_allowed(NULL, true));
}

void test_authed_allows_everything_registered(void) {
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.getMessages", true));
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.setAuthToken", true));
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.otaUpdate", true));
    TEST_ASSERT_TRUE(rpc_auth_method_allowed("bramble.ping", true));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ct_strcmp_match);
    RUN_TEST(test_ct_strcmp_mismatch);
    RUN_TEST(test_ct_strcmp_length_mismatch);
    RUN_TEST(test_ct_strcmp_both_empty);
    RUN_TEST(test_ct_strcmp_one_empty_a);
    RUN_TEST(test_ct_strcmp_one_empty_b);
    RUN_TEST(test_unauth_allows_ping);
    RUN_TEST(test_unauth_allows_get_version);
    RUN_TEST(test_unauth_denies_get_auth_token);
    RUN_TEST(test_unauth_denies_set_auth_token);
    RUN_TEST(test_unauth_denies_ota_update);
    RUN_TEST(test_unauth_denies_get_messages);
    RUN_TEST(test_unauth_denies_unknown_method);
    RUN_TEST(test_unauth_denies_prefix_lookalike);
    RUN_TEST(test_unauth_denies_null_method);
    RUN_TEST(test_authed_allows_everything_registered);
    return UNITY_END();
}
