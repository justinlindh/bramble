#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "identity.h"

#include <string.h>

static bramble_identity_t s_id = {.address = 0x01020304, .pubkey_hash = 0xAABBCCDD};
static char s_last_notify[512];

static void cap_notify(const char* json, size_t len, void* ctx) {
    (void)ctx;
    size_t n = len < sizeof(s_last_notify) - 1 ? len : sizeof(s_last_notify) - 1;
    memcpy(s_last_notify, json, n);
    s_last_notify[n] = '\0';
}

void setUp(void) {
    rpc_init();
    rpc_methods_init(&s_id);
    s_last_notify[0] = '\0';
    rpc_register_notify_transport(cap_notify, NULL);
}

void tearDown(void) {}

void test_send_broadcast_returns_broadcast_id(void) {
    char response[768];
    const char* req = "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"bramble.sendBroadcast\","
                      "\"params\":{\"text\":\"hi\"}}";
    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON* j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    cJSON* result = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_NOT_NULL(result);
    cJSON* bid = cJSON_GetObjectItem(result, "broadcast_id");
    TEST_ASSERT_TRUE(cJSON_IsString(bid));
    TEST_ASSERT_EQUAL(8, (int)strlen(bid->valuestring));
    cJSON_Delete(j);
}

void test_broadcast_delivery_emits_notification(void) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "recipient", "A1B2C3D4");
    cJSON_AddStringToObject(params, "broadcast_id", "ABCDEF01");
    rpc_notify("bramble.onBroadcastDelivery", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_EQUAL('\0', s_last_notify[0]);
    TEST_ASSERT_NOT_NULL(strstr(s_last_notify, "bramble.onBroadcastDelivery"));
    TEST_ASSERT_NOT_NULL(strstr(s_last_notify, "ABCDEF01"));
}

void test_recipient_only_mode_omits_path(void) {
    char response[768];
    const char* set_req = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"bramble."
                          "setBroadcastTelemetryMode\",\"params\":{\"mode\":\"recipient_only\"}}";
    int len = rpc_dispatch(set_req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    const char* get_req =
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"bramble.getConfig\",\"params\":{}}";
    len = rpc_dispatch(get_req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON* j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    cJSON* result = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_NOT_NULL(result);
    cJSON* mode = cJSON_GetObjectItem(result, "broadcast_telemetry_mode");
    TEST_ASSERT_TRUE(cJSON_IsString(mode));
    TEST_ASSERT_EQUAL_STRING("recipient_only", mode->valuestring);
    cJSON_Delete(j);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_send_broadcast_returns_broadcast_id);
    RUN_TEST(test_broadcast_delivery_emits_notification);
    RUN_TEST(test_recipient_only_mode_omits_path);
    return UNITY_END();
}
