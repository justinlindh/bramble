#include "unity.h"
#include "../main/ct_strcmp.h"
#include "rpc_auth.h"

void setUp(void) {}
void tearDown(void) {}

void test_ct_strcmp_match(void) { TEST_ASSERT_EQUAL(0, ct_strcmp("abc", "abc")); }

void test_ct_strcmp_mismatch(void) { TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("abc", "abd")); }

void test_ct_strcmp_length_mismatch(void) { TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("abc", "ab")); }

void test_ct_strcmp_both_empty(void) { TEST_ASSERT_EQUAL(0, ct_strcmp("", "")); }

void test_ct_strcmp_one_empty_a(void) { TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("a", "")); }

void test_ct_strcmp_one_empty_b(void) { TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("", "a")); }

void test_ct_strcmp_long_match(void) {
    /* 64-char token, typical generated size x2 */
    const char* t = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    TEST_ASSERT_EQUAL(0, ct_strcmp(t, t));
}

void test_ct_strcmp_long_prefix_mismatch(void) {
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("0123456789ABCDEF", "0123456789ABCDEFX"));
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp("0123456789ABCDEFX", "0123456789ABCDEF"));
}

void test_ct_strcmp_at_bound_match(void) {
    /* exactly CT_STRCMP_BOUND chars on both sides */
    char a[CT_STRCMP_BOUND + 1];
    char b[CT_STRCMP_BOUND + 1];
    memset(a, 'q', CT_STRCMP_BOUND);
    memset(b, 'q', CT_STRCMP_BOUND);
    a[CT_STRCMP_BOUND] = '\0';
    b[CT_STRCMP_BOUND] = '\0';
    TEST_ASSERT_EQUAL(0, ct_strcmp(a, b));
    b[CT_STRCMP_BOUND - 1] = 'z';
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp(a, b));
}

void test_ct_strcmp_oversized_attacker_input_rejected(void) {
    /* Attacker input longer than the bound vs an in-bounds secret must
     * not match (documented truncation behavior). */
    char attacker[CT_STRCMP_BOUND + 32];
    memset(attacker, 'q', sizeof(attacker) - 1);
    attacker[sizeof(attacker) - 1] = '\0';
    char secret[32];
    memset(secret, 'q', sizeof(secret) - 1);
    secret[sizeof(secret) - 1] = '\0';
    TEST_ASSERT_NOT_EQUAL(0, ct_strcmp(attacker, secret));
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

/* ── Notification gating (server push goes only to authed clients) ──── */

void test_notify_unauth_connection_receives_nothing(void) {
    rpc_notify_client_t clients[] = {{.fd = 7, .authenticated = false}};
    int out[4];
    TEST_ASSERT_EQUAL(0, rpc_auth_notify_filter(clients, 1, false, out, 4));
}

void test_notify_mixed_clients_only_authed_selected(void) {
    rpc_notify_client_t clients[] = {
        {.fd = 3, .authenticated = true},
        {.fd = 4, .authenticated = false},
        {.fd = 5, .authenticated = true},
        {.fd = 6, .authenticated = false},
    };
    int out[4];
    int n = rpc_auth_notify_filter(clients, 4, false, out, 4);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(3, out[0]);
    TEST_ASSERT_EQUAL(5, out[1]);
}

void test_notify_opt_out_device_broadcasts_to_all(void) {
    rpc_notify_client_t clients[] = {
        {.fd = 3, .authenticated = true},
        {.fd = 4, .authenticated = false},
    };
    int out[4];
    TEST_ASSERT_EQUAL(2, rpc_auth_notify_filter(clients, 2, true, out, 4));
}

void test_notify_allowed_truth_table(void) {
    TEST_ASSERT_TRUE(rpc_auth_notify_allowed(true, false));
    TEST_ASSERT_TRUE(rpc_auth_notify_allowed(true, true));
    TEST_ASSERT_TRUE(rpc_auth_notify_allowed(false, true));
    TEST_ASSERT_FALSE(rpc_auth_notify_allowed(false, false));
}

void test_notify_filter_respects_max_out(void) {
    rpc_notify_client_t clients[] = {
        {.fd = 1, .authenticated = true},
        {.fd = 2, .authenticated = true},
        {.fd = 3, .authenticated = true},
    };
    int out[2];
    TEST_ASSERT_EQUAL(2, rpc_auth_notify_filter(clients, 3, false, out, 2));
}

void test_notify_filter_null_safe(void) {
    int out[2];
    rpc_notify_client_t clients[] = {{.fd = 1, .authenticated = true}};
    TEST_ASSERT_EQUAL(0, rpc_auth_notify_filter(NULL, 1, false, out, 2));
    TEST_ASSERT_EQUAL(0, rpc_auth_notify_filter(clients, 1, false, NULL, 2));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ct_strcmp_match);
    RUN_TEST(test_ct_strcmp_mismatch);
    RUN_TEST(test_ct_strcmp_length_mismatch);
    RUN_TEST(test_ct_strcmp_both_empty);
    RUN_TEST(test_ct_strcmp_one_empty_a);
    RUN_TEST(test_ct_strcmp_one_empty_b);
    RUN_TEST(test_ct_strcmp_long_match);
    RUN_TEST(test_ct_strcmp_long_prefix_mismatch);
    RUN_TEST(test_ct_strcmp_at_bound_match);
    RUN_TEST(test_ct_strcmp_oversized_attacker_input_rejected);
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
    RUN_TEST(test_notify_unauth_connection_receives_nothing);
    RUN_TEST(test_notify_mixed_clients_only_authed_selected);
    RUN_TEST(test_notify_opt_out_device_broadcasts_to_all);
    RUN_TEST(test_notify_allowed_truth_table);
    RUN_TEST(test_notify_filter_respects_max_out);
    RUN_TEST(test_notify_filter_null_safe);
    return UNITY_END();
}
