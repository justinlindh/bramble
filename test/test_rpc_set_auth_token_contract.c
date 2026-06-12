#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "rpc_auth.h"
#include <stdio.h>
#include <string.h>

extern bool g_nvs_allow_open;

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

void setUp(void) {
    g_nvs_allow_open = true;
    rpc_init();
    rpc_methods_init(&s_id);
}

void tearDown(void) {}

static int set_token_and_get_code(const char *token, char *response, size_t resp_len) {
    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setAuthToken\","
             "\"params\":{\"token\":\"%s\"}}",
             token);
    int len = rpc_dispatch(req, response, resp_len);
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *err = cJSON_GetObjectItem(j, "error");
    int code = 0;
    if (err) {
        code = cJSON_GetObjectItem(err, "code")->valueint;
    } else {
        cJSON *res = cJSON_GetObjectItem(j, "result");
        TEST_ASSERT_NOT_NULL(res);
        TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    }
    cJSON_Delete(j);
    return code;
}

void test_set_auth_token_rejects_short_token(void) {
    char response[512];
    /* 15 bytes: one short of the floor */
    TEST_ASSERT_EQUAL(-32602, set_token_and_get_code("abcdefghijklmno", response, sizeof(response)));
}

void test_set_auth_token_rejects_one_char_token(void) {
    char response[512];
    TEST_ASSERT_EQUAL(-32602, set_token_and_get_code("x", response, sizeof(response)));
}

void test_set_auth_token_accepts_16_byte_token(void) {
    char response[512];
    TEST_ASSERT_EQUAL(0, set_token_and_get_code("abcdefghijklmnop", response, sizeof(response)));
}

void test_set_auth_token_accepts_generated_style_token(void) {
    char response[512];
    TEST_ASSERT_EQUAL(0, set_token_and_get_code("0123456789ABCDEF0123456789ABCDEF", response,
                                                sizeof(response)));
}

void test_set_auth_token_accepts_empty_token_as_opt_out(void) {
    /* Empty = explicit auth disable, not a weak credential */
    char response[512];
    TEST_ASSERT_EQUAL(0, set_token_and_get_code("", response, sizeof(response)));
}

void test_token_len_floor_helper(void) {
    TEST_ASSERT_TRUE(rpc_auth_token_len_ok(0));
    TEST_ASSERT_FALSE(rpc_auth_token_len_ok(1));
    TEST_ASSERT_FALSE(rpc_auth_token_len_ok(15));
    TEST_ASSERT_TRUE(rpc_auth_token_len_ok(16));
    TEST_ASSERT_TRUE(rpc_auth_token_len_ok(32));
    TEST_ASSERT_TRUE(rpc_auth_token_len_ok(127));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_auth_token_rejects_short_token);
    RUN_TEST(test_set_auth_token_rejects_one_char_token);
    RUN_TEST(test_set_auth_token_accepts_16_byte_token);
    RUN_TEST(test_set_auth_token_accepts_generated_style_token);
    RUN_TEST(test_set_auth_token_accepts_empty_token_as_opt_out);
    RUN_TEST(test_token_len_floor_helper);
    return UNITY_END();
}
