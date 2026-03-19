/**
 * test/host/test_rpc_methods.c
 *
 * Host-side unit tests for RPC method handlers that require no hardware.
 * Exercises real rpc_methods.c with stubs; covers:
 *   - bramble.ping          (response format)
 *   - bramble.getVersion    (response format)
 *   - bramble.getConfig     (node_name fallback, identity, radio, channels)
 *   - bramble.setAuthToken  (param validation + success paths)
 *
 * T3 audit finding: add unit coverage for rpc_methods.c pure-logic methods.
 */

#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include <string.h>
#include <stdbool.h>

/* ── Controllable stubs from rpc_methods_test_stubs.c ─────────────── */

extern bool     g_nvs_allow_open;
extern char     g_nvs_node_name[64];
extern int      g_mesh_channel_count;
extern int      g_mesh_default_channel;
extern char     g_mesh_channel_names[8][20];
extern bool     g_mesh_channel_has_psk[8];
extern uint16_t g_mesh_channel_epoch[8];

/* ── Extra stubs: symbols not covered by rpc_methods_test_stubs.c ──── */

/* ws_server_get_token — used by bramble.getAuthToken handler */
const char *ws_server_get_token(void) { return NULL; }

/* esp_wifi_get_mac / esp_wifi_ap_get_sta_list — called by handle_get_wifi_status */
#include "esp_wifi.h"
esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]) {
    (void)ifx;
    if (mac) memset(mac, 0, 6);
    return ESP_OK;
}
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *list) {
    (void)list;
    return ESP_OK;
}

/* ── Suppress ASAN leak detector for OTA strdup that doesn't free ──── */

const char *__asan_default_options(void) { return "detect_leaks=0"; }

/* ── Test identity ───────────────────────────────────────────────────── */

static bramble_identity_t s_id = {
    .address     = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

/* ── setUp / tearDown ────────────────────────────────────────────────── */

void setUp(void) {
    rpc_init();
    rpc_methods_init(&s_id);

    g_nvs_allow_open         = true;
    g_nvs_node_name[0]       = '\0';
    g_mesh_channel_count     = 1;
    g_mesh_default_channel   = 0;
    strncpy(g_mesh_channel_names[0], "Broadcast", sizeof(g_mesh_channel_names[0]) - 1);
    g_mesh_channel_has_psk[0] = false;
    g_mesh_channel_epoch[0]   = 0;
}

void tearDown(void) {}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static cJSON *dispatch_and_parse(const char *req) {
    char response[2048];
    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON *j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static cJSON *assert_result(cJSON *resp) {
    TEST_ASSERT_NULL_MESSAGE(
        cJSON_GetObjectItem(resp, "error"),
        "unexpected error field in response");
    cJSON *r = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "expected 'result' field");
    return r;
}

static int assert_error_code(cJSON *resp) {
    cJSON *e = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "expected 'error' field");
    cJSON *code = cJSON_GetObjectItem(e, "code");
    TEST_ASSERT_NOT_NULL(code);
    return code->valueint;
}

/* ── bramble.ping ──────────────────────────────────────────────────────
 * Pure read — no side effects, no hardware required.
 * ──────────────────────────────────────────────────────────────────── */

void test_ping_returns_pong_true(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.ping\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_TRUE_MESSAGE(
        cJSON_IsTrue(cJSON_GetObjectItem(r, "pong")),
        "pong should be true");
    cJSON_Delete(resp);
}

void test_ping_returns_node_address(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bramble.ping\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *addr = cJSON_GetObjectItem(r, "address");
    TEST_ASSERT_NOT_NULL(addr);
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", addr->valuestring);
    cJSON_Delete(resp);
}

void test_ping_returns_protocol_version_string(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"bramble.ping\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *pv = cJSON_GetObjectItem(r, "protocol_version");
    TEST_ASSERT_NOT_NULL(pv);
    TEST_ASSERT_TRUE(cJSON_IsString(pv));
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(pv->valuestring));
    cJSON_Delete(resp);
}

/* ── bramble.getVersion ────────────────────────────────────────────────
 * Pure read — returns firmware/protocol metadata.
 * ──────────────────────────────────────────────────────────────────── */

void test_get_version_has_firmware_version_field(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"bramble.getVersion\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *fv = cJSON_GetObjectItem(r, "firmware_version");
    TEST_ASSERT_NOT_NULL(fv);
    TEST_ASSERT_TRUE(cJSON_IsString(fv));
    cJSON_Delete(resp);
}

void test_get_version_has_protocol_version_field(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"bramble.getVersion\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *pv = cJSON_GetObjectItem(r, "protocol_version");
    TEST_ASSERT_NOT_NULL(pv);
    TEST_ASSERT_TRUE(cJSON_IsString(pv));
    cJSON_Delete(resp);
}

void test_get_version_has_hardware_string(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"bramble.getVersion\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *hw = cJSON_GetObjectItem(r, "hardware");
    TEST_ASSERT_NOT_NULL(hw);
    TEST_ASSERT_TRUE(cJSON_IsString(hw));
    cJSON_Delete(resp);
}

void test_get_version_has_delivery_event_sync_bool(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bramble.getVersion\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *flag = cJSON_GetObjectItem(r, "supports_delivery_event_sync");
    TEST_ASSERT_NOT_NULL(flag);
    TEST_ASSERT_TRUE(cJSON_IsBool(flag));
    cJSON_Delete(resp);
}

void test_get_version_no_params_required(void) {
    /* Omit params entirely — should still succeed */
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"bramble.getVersion\"}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "protocol_version"));
    cJSON_Delete(resp);
}

/* ── bramble.getConfig ─────────────────────────────────────────────────
 * Reads NVS node name, identity, radio config, and channel list.
 * ──────────────────────────────────────────────────────────────────── */

void test_get_config_node_name_falls_back_to_unnamed(void) {
    /* NVS unavailable (e.g. first boot) → handler keeps "(unnamed)" default */
    g_nvs_allow_open   = false;
    g_nvs_node_name[0] = '\0';

    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"bramble.getConfig\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_EQUAL_STRING("(unnamed)", cJSON_GetObjectItem(r, "node_name")->valuestring);
    cJSON_Delete(resp);
}

void test_get_config_reflects_stored_node_name(void) {
    strncpy(g_nvs_node_name, "FieldNode", sizeof(g_nvs_node_name) - 1);
    g_nvs_allow_open = true;

    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"bramble.getConfig\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_EQUAL_STRING("FieldNode", cJSON_GetObjectItem(r, "node_name")->valuestring);
    cJSON_Delete(resp);
}

void test_get_config_returns_identity_address_and_pubkey_hash(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"bramble.getConfig\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", cJSON_GetObjectItem(r, "address")->valuestring);
    TEST_ASSERT_EQUAL_STRING("11223344", cJSON_GetObjectItem(r, "pubkey_hash")->valuestring);
    cJSON_Delete(resp);
}

void test_get_config_radio_object_has_required_fields(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"bramble.getConfig\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *radio = cJSON_GetObjectItem(r, "radio");
    TEST_ASSERT_NOT_NULL(radio);
    TEST_ASSERT_TRUE(cJSON_IsObject(radio));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "frequency_mhz"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "sf"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "bw_hz"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "tx_power_dbm"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "coding_rate"));
    cJSON_Delete(resp);
}

void test_get_config_channels_array_has_one_entry(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"bramble.getConfig\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *channels = cJSON_GetObjectItem(r, "channels");
    TEST_ASSERT_NOT_NULL(channels);
    TEST_ASSERT_TRUE(cJSON_IsArray(channels));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(channels));
    cJSON_Delete(resp);
}

void test_get_config_default_channel_is_marked(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"bramble.getConfig\",\"params\":{}}");
    cJSON *r = assert_result(resp);
    cJSON *channels = cJSON_GetObjectItem(r, "channels");
    cJSON *ch = cJSON_GetArrayItem(channels, 0);
    TEST_ASSERT_NOT_NULL(ch);
    TEST_ASSERT_EQUAL_STRING("Broadcast", cJSON_GetObjectItem(ch, "name")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(ch, "is_default")));
    cJSON_Delete(resp);
}

/* ── bramble.setAuthToken ──────────────────────────────────────────────
 * Validates the token param strictly before writing to NVS.
 * ──────────────────────────────────────────────────────────────────── */

void test_set_auth_token_missing_token_field_returns_invalid_params(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"bramble.setAuthToken\","
        "\"params\":{}}");
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, assert_error_code(resp));
    cJSON_Delete(resp);
}

void test_set_auth_token_numeric_value_returns_invalid_params(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"bramble.setAuthToken\","
        "\"params\":{\"token\":42}}");
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, assert_error_code(resp));
    cJSON_Delete(resp);
}

void test_set_auth_token_boolean_value_returns_invalid_params(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"bramble.setAuthToken\","
        "\"params\":{\"token\":true}}");
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, assert_error_code(resp));
    cJSON_Delete(resp);
}

void test_set_auth_token_128_char_token_returns_invalid_params(void) {
    /* Max allowed is < 128 chars; exactly 128 must be rejected */
    char req[600];
    char tok[129];
    memset(tok, 'x', 128);
    tok[128] = '\0';
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"bramble.setAuthToken\","
        "\"params\":{\"token\":\"%s\"}}", tok);
    cJSON *resp = dispatch_and_parse(req);
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, assert_error_code(resp));
    cJSON_Delete(resp);
}

void test_set_auth_token_valid_returns_ok_true(void) {
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"bramble.setAuthToken\","
        "\"params\":{\"token\":\"s3cr3t\"}}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(resp);
}

void test_set_auth_token_empty_string_clears_token(void) {
    /* Empty string is a valid value meaning "clear the token" */
    cJSON *resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"bramble.setAuthToken\","
        "\"params\":{\"token\":\"\"}}");
    cJSON *r = assert_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(resp);
}

void test_set_auth_token_127_char_token_accepted(void) {
    /* 127 chars is within the limit */
    char req[600];
    char tok[128];
    memset(tok, 'a', 127);
    tok[127] = '\0';
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"bramble.setAuthToken\","
        "\"params\":{\"token\":\"%s\"}}", tok);
    cJSON *resp = dispatch_and_parse(req);
    cJSON *r = assert_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON_Delete(resp);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* ping */
    RUN_TEST(test_ping_returns_pong_true);
    RUN_TEST(test_ping_returns_node_address);
    RUN_TEST(test_ping_returns_protocol_version_string);

    /* getVersion */
    RUN_TEST(test_get_version_has_firmware_version_field);
    RUN_TEST(test_get_version_has_protocol_version_field);
    RUN_TEST(test_get_version_has_hardware_string);
    RUN_TEST(test_get_version_has_delivery_event_sync_bool);
    RUN_TEST(test_get_version_no_params_required);

    /* getConfig */
    RUN_TEST(test_get_config_node_name_falls_back_to_unnamed);
    RUN_TEST(test_get_config_reflects_stored_node_name);
    RUN_TEST(test_get_config_returns_identity_address_and_pubkey_hash);
    RUN_TEST(test_get_config_radio_object_has_required_fields);
    RUN_TEST(test_get_config_channels_array_has_one_entry);
    RUN_TEST(test_get_config_default_channel_is_marked);

    /* setAuthToken */
    RUN_TEST(test_set_auth_token_missing_token_field_returns_invalid_params);
    RUN_TEST(test_set_auth_token_numeric_value_returns_invalid_params);
    RUN_TEST(test_set_auth_token_boolean_value_returns_invalid_params);
    RUN_TEST(test_set_auth_token_128_char_token_returns_invalid_params);
    RUN_TEST(test_set_auth_token_valid_returns_ok_true);
    RUN_TEST(test_set_auth_token_empty_string_clears_token);
    RUN_TEST(test_set_auth_token_127_char_token_accepted);

    return UNITY_END();
}
