#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include <stdio.h>
#include <string.h>

extern bool g_nvs_allow_open;
extern bool g_ble_has_passkey_display;
extern int g_ble_pairing_config_changed_calls;

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

void setUp(void) {
    g_nvs_allow_open = true;
    g_ble_has_passkey_display = false;
    g_ble_pairing_config_changed_calls = 0;
    rpc_init();
    rpc_methods_init(&s_id);
    /* Reset store state between tests */
    char resp[512];
    rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.setBlePasskey\","
                 "\"params\":{\"passkey\":null}}",
                 resp, sizeof(resp));
    g_ble_pairing_config_changed_calls = 0;
}
void tearDown(void) {}

static cJSON* call(const char* method, const char* params_json, char* resp, size_t resp_len) {
    char req[512];
    snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
             method, params_json);
    int len = rpc_dispatch(req, resp, resp_len);
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON* j = cJSON_Parse(resp);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

void test_get_ble_security_default_just_works(void) {
    char resp[512];
    cJSON* j = call("bramble.getBleSecurity", "{}", resp, sizeof(resp));
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("just-works",
                             cJSON_GetObjectItem(res, "mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "staticPasskeySet")));
    cJSON_Delete(j);
}

void test_set_passkey_switches_mode_and_wipes_bonds(void) {
    char resp[512];
    cJSON* j = call("bramble.setBlePasskey", "{\"passkey\":\"042042\"}", resp, sizeof(resp));
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    TEST_ASSERT_EQUAL_STRING("static-passkey",
                             cJSON_GetObjectItem(res, "mode")->valuestring);
    cJSON_Delete(j);
    TEST_ASSERT_EQUAL_INT(1, g_ble_pairing_config_changed_calls);

    j = call("bramble.getBleSecurity", "{}", resp, sizeof(resp));
    res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_EQUAL_STRING("static-passkey",
                             cJSON_GetObjectItem(res, "mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "staticPasskeySet")));
    /* Write-only: the value never appears in any response */
    TEST_ASSERT_NULL(strstr(resp, "042042"));
    cJSON_Delete(j);
}

void test_clear_passkey_returns_to_just_works(void) {
    char resp[512];
    cJSON* j = call("bramble.setBlePasskey", "{\"passkey\":\"123456\"}", resp, sizeof(resp));
    cJSON_Delete(j);
    j = call("bramble.setBlePasskey", "{\"passkey\":null}", resp, sizeof(resp));
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    TEST_ASSERT_EQUAL_STRING("just-works",
                             cJSON_GetObjectItem(res, "mode")->valuestring);
    cJSON_Delete(j);
    TEST_ASSERT_EQUAL_INT(2, g_ble_pairing_config_changed_calls);
}

void test_empty_string_clears_too(void) {
    char resp[512];
    cJSON* j = call("bramble.setBlePasskey", "{\"passkey\":\"\"}", resp, sizeof(resp));
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    cJSON_Delete(j);
}

void test_invalid_passkeys_rejected(void) {
    const char* bad[] = {"{\"passkey\":\"12345\"}", "{\"passkey\":\"1234567\"}",
                         "{\"passkey\":\"12345a\"}", "{\"passkey\":123456}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char resp[512];
        cJSON* j = call("bramble.setBlePasskey", bad[i], resp, sizeof(resp));
        cJSON* res = cJSON_GetObjectItem(j, "result");
        TEST_ASSERT_NOT_NULL_MESSAGE(res, bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsFalse(cJSON_GetObjectItem(res, "ok")), bad[i]);
        cJSON_Delete(j);
    }
    TEST_ASSERT_EQUAL_INT(0, g_ble_pairing_config_changed_calls);
}

void test_rejected_on_display_boards(void) {
    g_ble_has_passkey_display = true;
    char resp[512];
    cJSON* j = call("bramble.setBlePasskey", "{\"passkey\":\"123456\"}", resp, sizeof(resp));
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(res, "ok")));
    cJSON_Delete(j);
    TEST_ASSERT_EQUAL_INT(0, g_ble_pairing_config_changed_calls);

    j = call("bramble.getBleSecurity", "{}", resp, sizeof(resp));
    res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_EQUAL_STRING("passkey-display",
                             cJSON_GetObjectItem(res, "mode")->valuestring);
    cJSON_Delete(j);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_ble_security_default_just_works);
    RUN_TEST(test_set_passkey_switches_mode_and_wipes_bonds);
    RUN_TEST(test_clear_passkey_returns_to_just_works);
    RUN_TEST(test_empty_string_clears_too);
    RUN_TEST(test_invalid_passkeys_rejected);
    RUN_TEST(test_rejected_on_display_boards);
    return UNITY_END();
}
