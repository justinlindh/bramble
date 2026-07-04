/**
 * test_rpc_methods_firmware.c
 *
 * Tests against the real rpc_methods.c handlers (not mock handlers).
 * Covers: getStatus, getNeighbors, sendMessage, sendBroadcast,
 *         otaUpdate, setNodeName.
 */
#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include <string.h>
#include <stdbool.h>

/* Controllable stubs from rpc_methods_test_stubs.c */
extern uint32_t g_stub_send_message_return;
extern uint32_t g_stub_send_channel_return;
extern int g_stub_send_broadcast_return;
extern uint32_t g_stub_last_broadcast_id;
extern bool g_nvs_allow_open;
extern char g_nvs_node_name[64];

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

void setUp(void) {
    rpc_init();
    rpc_methods_init(&s_id);
    g_stub_send_message_return = 0x12345678;
    g_stub_send_channel_return = 0x12345678;
    g_stub_send_broadcast_return = 0;
    g_stub_last_broadcast_id = 0xABCDEF01;
    g_nvs_allow_open = true;
    g_nvs_node_name[0] = '\0';
}

void tearDown(void) {}

/* Suppress known OTA strdup leak in test environment */
const char* __asan_default_options(void) { return "detect_leaks=0"; }

/* ── Helpers ──────────────────────────────────────────────────────── */

static cJSON* dispatch_and_parse(const char* req) {
    char response[2048];
    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON* j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static cJSON* get_result(cJSON* resp) {
    cJSON* r = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

static cJSON* get_error(cJSON* resp) {
    cJSON* e = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(e);
    return e;
}

/* ── 1. getStatus ─────────────────────────────────────────────────── */

void test_get_status_returns_expected_fields(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getStatus\",\"params\":{}}");
    cJSON* r = get_result(resp);

    /* address should be our identity hex */
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", cJSON_GetObjectItem(r, "address")->valuestring);

    /* Verify presence of key fields */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "firmware_version"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "protocol_version"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "hardware"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "radio_ok"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "uptime_s"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "free_heap"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "battery_mv"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "battery_pct"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "peers"));

    cJSON_Delete(resp);
}

/* ── 2. getNeighbors ──────────────────────────────────────────────── */

void test_get_neighbors_empty_table(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bramble.getNeighbors\",\"params\":{}}");
    cJSON* r = get_result(resp);
    cJSON* arr = cJSON_GetObjectItem(r, "neighbors");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(arr));
    cJSON_Delete(resp);
}

/* ── 3. sendMessage ───────────────────────────────────────────────── */

void test_send_message_missing_dest(void) {
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"bramble."
                                     "sendMessage\",\"params\":{\"text\":\"hello\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_send_message_missing_text(void) {
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"bramble."
                                     "sendMessage\",\"params\":{\"dest\":\"AABBCCDD\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_send_message_too_long(void) {
    /* FRAGMENTED_MAX_BYTES = 154 * 4 = 616. Build a message > 616 chars */
    char params[800];
    char big_msg[700];
    memset(big_msg, 'A', 699);
    big_msg[699] = '\0';
    snprintf(params, sizeof(params),
             "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"bramble.sendMessage\","
             "\"params\":{\"dest\":\"AABBCCDD\",\"text\":\"%s\"}}",
             big_msg);

    cJSON* resp = dispatch_and_parse(params);
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_send_message_valid(void) {
    g_stub_send_message_return = 0xDEADBEEF;
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"bramble.sendMessage\","
                           "\"params\":{\"dest\":\"AABBCCDD\",\"text\":\"hello\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_EQUAL_STRING("sent", cJSON_GetObjectItem(r, "status")->valuestring);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "packetId"));
    TEST_ASSERT_EQUAL_STRING("DEADBEEF", cJSON_GetObjectItem(r, "packetId")->valuestring);
    cJSON_Delete(resp);
}

void test_send_message_radio_failure(void) {
    g_stub_send_message_return = 0; /* 0 = failure */
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bramble.sendMessage\","
                           "\"params\":{\"dest\":\"AABBCCDD\",\"text\":\"hello\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(resp);
}

/* ── 4. sendBroadcast ─────────────────────────────────────────────── */

void test_send_broadcast_missing_text(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"bramble.sendBroadcast\",\"params\":{}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_send_broadcast_valid(void) {
    g_stub_send_broadcast_return = 0; /* 0 = success */
    g_stub_last_broadcast_id = 0xCAFEBABE;
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"bramble.sendBroadcast\","
                           "\"params\":{\"text\":\"alert everyone\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_EQUAL_STRING("CAFEBABE", cJSON_GetObjectItem(r, "broadcast_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("sent", cJSON_GetObjectItem(r, "status")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "broadcast")));
    cJSON_Delete(resp);
}

/* ── 5. otaUpdate / otaGetOrigin / otaSetOrigin ───────────────────── */

extern char g_ota_last_url[256];
extern bool g_ota_last_allow_downgrade;
extern int g_ota_wifi_start_calls;
extern char g_ota_origin_stub[256];
extern bool g_ota_origin_overridden_stub;

void test_ota_update_missing_path(void) {
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":"
                                     "\"bramble.otaUpdate\",\"params\":{}}");
    cJSON* r = get_result(resp);
    /* Returns ok:false in result, not a JSON-RPC error */
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetObjectItem(r, "error")->valuestring, "path"));
    cJSON_Delete(resp);
}

void test_ota_update_raw_url_rejected(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"bramble.otaUpdate\","
                           "\"params\":{\"url\":\"https://evil.example/firmware.bin\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetObjectItem(r, "error")->valuestring, "raw URLs"));
    cJSON_Delete(resp);
}

void test_ota_update_absolute_url_in_path_rejected(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"bramble.otaUpdate\","
                           "\"params\":{\"path\":\"https://evil.example/firmware.bin\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetObjectItem(r, "error")->valuestring, "invalid"));
    cJSON_Delete(resp);
}

void test_ota_update_traversal_path_rejected(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"bramble.otaUpdate\","
                           "\"params\":{\"path\":\"stable/../../secrets\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(resp);
}

void test_ota_update_resolves_against_origin_and_already_in_progress(void) {
    /* After a successful OTA start (xTaskCreate stub doesn't run the task),
     * s_ota_in_progress stays true, so a second call should be rejected. */
    cJSON* resp1 =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"bramble.otaUpdate\","
                           "\"params\":{\"path\":\"stable/v1.4.0/heltec-v3/bramble.bin\","
                           "\"allow_downgrade\":true}}");
    cJSON* r1 = get_result(resp1);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r1, "ok")));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r1, "partition"));
    TEST_ASSERT_EQUAL_STRING("https://bramblemesh.org/ota/stable/v1.4.0/heltec-v3/bramble.bin",
                             cJSON_GetObjectItem(r1, "url")->valuestring);
    cJSON_Delete(resp1);

    /* Second: should fail with "already in progress" */
    cJSON* resp2 =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"bramble.otaUpdate\","
                           "\"params\":{\"path\":\"stable/v1.4.0/heltec-v3/bramble.bin\"}}");
    cJSON* r2 = get_result(resp2);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r2, "ok")));
    TEST_ASSERT_NOT_NULL(
        strstr(cJSON_GetObjectItem(r2, "error")->valuestring, "already in progress"));
    cJSON_Delete(resp2);
}

void test_ota_get_origin_reports_default(void) {
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":"
                                     "\"bramble.otaGetOrigin\",\"params\":{}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("https://bramblemesh.org/ota/",
                             cJSON_GetObjectItem(r, "origin")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "overridden")));
    cJSON_Delete(resp);
}

void test_ota_set_origin_validates_policy(void) {
    /* Foreign scheme rejected */
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"bramble.otaSetOrigin\","
                           "\"params\":{\"origin\":\"ftp://mirror.example/\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(resp);

    /* Valid https origin accepted */
    resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"bramble.otaSetOrigin\","
                              "\"params\":{\"origin\":\"https://mirror.example/ota/\"}}");
    r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("https://mirror.example/ota/",
                             cJSON_GetObjectItem(r, "origin")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "overridden")));
    cJSON_Delete(resp);

    /* Reset returns to default */
    resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"bramble.otaSetOrigin\","
                              "\"params\":{\"reset\":true}}");
    r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("https://bramblemesh.org/ota/",
                             cJSON_GetObjectItem(r, "origin")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "overridden")));
    cJSON_Delete(resp);
}

/* ── 6. setNodeName ───────────────────────────────────────────────── */

void test_set_node_name_too_long(void) {
    /* BRAMBLE_NODE_NAME_MAX = 32, so 33 chars should fail */
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"bramble.setNodeName\","
                           "\"params\":{\"name\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_node_name_empty(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"bramble.setNodeName\","
                           "\"params\":{\"name\":\"\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_node_name_missing(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"bramble.setNodeName\","
                           "\"params\":{}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_node_name_valid(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"bramble.setNodeName\","
                           "\"params\":{\"name\":\"MyNode\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("MyNode", cJSON_GetObjectItem(r, "name")->valuestring);
    /* Verify NVS was written */
    TEST_ASSERT_EQUAL_STRING("MyNode", g_nvs_node_name);
    cJSON_Delete(resp);
}

void test_set_node_name_max_length(void) {
    /* Exactly 32 chars should succeed */
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"bramble.setNodeName\","
                           "\"params\":{\"name\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(resp);
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* getStatus */
    RUN_TEST(test_get_status_returns_expected_fields);

    /* getNeighbors */
    RUN_TEST(test_get_neighbors_empty_table);

    /* sendMessage */
    RUN_TEST(test_send_message_missing_dest);
    RUN_TEST(test_send_message_missing_text);
    RUN_TEST(test_send_message_too_long);
    RUN_TEST(test_send_message_valid);
    RUN_TEST(test_send_message_radio_failure);

    /* sendBroadcast */
    RUN_TEST(test_send_broadcast_missing_text);
    RUN_TEST(test_send_broadcast_valid);

    /* otaUpdate / otaGetOrigin / otaSetOrigin */
    RUN_TEST(test_ota_update_missing_path);
    RUN_TEST(test_ota_update_raw_url_rejected);
    RUN_TEST(test_ota_update_absolute_url_in_path_rejected);
    RUN_TEST(test_ota_update_traversal_path_rejected);
    RUN_TEST(test_ota_update_resolves_against_origin_and_already_in_progress);
    RUN_TEST(test_ota_get_origin_reports_default);
    RUN_TEST(test_ota_set_origin_validates_policy);

    /* setNodeName */
    RUN_TEST(test_set_node_name_too_long);
    RUN_TEST(test_set_node_name_empty);
    RUN_TEST(test_set_node_name_missing);
    RUN_TEST(test_set_node_name_valid);
    RUN_TEST(test_set_node_name_max_length);

    return UNITY_END();
}
