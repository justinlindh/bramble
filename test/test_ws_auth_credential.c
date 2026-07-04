#include "unity.h"
#include "ws_auth_credential.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_bearer_token_extracted(void) {
    char out[64] = {0};
    TEST_ASSERT_TRUE(ws_auth_extract_token("Bearer ABC123", NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ABC123", out);
}

void test_bearer_trims_surrounding_space(void) {
    char out[64] = {0};
    TEST_ASSERT_TRUE(ws_auth_extract_token("Bearer   ABC123  ", NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ABC123", out);
}

void test_subprotocol_token_extracted(void) {
    char out[64] = {0};
    TEST_ASSERT_TRUE(
        ws_auth_extract_token(NULL, "bramble.v1.auth.DEADBEEF, bramble.v1", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("DEADBEEF", out);
}

void test_subprotocol_token_any_order(void) {
    char out[64] = {0};
    TEST_ASSERT_TRUE(
        ws_auth_extract_token(NULL, "bramble.v1 , bramble.v1.auth.CAFE", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("CAFE", out);
}

void test_subprotocol_used_when_authorization_not_bearer(void) {
    char out[64] = {0};
    TEST_ASSERT_TRUE(ws_auth_extract_token("Basic xyz", "bramble.v1.auth.TOK", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TOK", out);
}

void test_non_auth_subprotocol_only_is_no_creds(void) {
    char out[64] = {0};
    TEST_ASSERT_FALSE(ws_auth_extract_token(NULL, "bramble.v1", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_empty_bearer_token_rejected(void) {
    char out[64] = {0};
    TEST_ASSERT_FALSE(ws_auth_extract_token("Bearer ", NULL, out, sizeof(out)));
}

void test_no_credentials_is_false(void) {
    char out[64] = {0};
    TEST_ASSERT_FALSE(ws_auth_extract_token(NULL, NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(ws_auth_extract_token("", "", out, sizeof(out)));
}

void test_token_overflowing_out_buffer_rejected(void) {
    char out[4] = {0};
    TEST_ASSERT_FALSE(ws_auth_extract_token("Bearer TOOLONGVALUE", NULL, out, sizeof(out)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bearer_token_extracted);
    RUN_TEST(test_bearer_trims_surrounding_space);
    RUN_TEST(test_subprotocol_token_extracted);
    RUN_TEST(test_subprotocol_token_any_order);
    RUN_TEST(test_subprotocol_used_when_authorization_not_bearer);
    RUN_TEST(test_non_auth_subprotocol_only_is_no_creds);
    RUN_TEST(test_empty_bearer_token_rejected);
    RUN_TEST(test_no_credentials_is_false);
    RUN_TEST(test_token_overflowing_out_buffer_rejected);
    return UNITY_END();
}
