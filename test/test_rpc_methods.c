#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include <stdbool.h>

void setUp(void) { rpc_init(); }
void tearDown(void) {}

static cJSON *parse_response(const char *resp) {
    cJSON *j = cJSON_Parse(resp);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("2.0", cJSON_GetObjectItem(j, "jsonrpc")->valuestring);
    return j;
}

/* Representative firmware-style handlers to validate parameter and error behavior. */
static int h_send_message(const cJSON *params, cJSON *result) {
    const cJSON *dest = cJSON_GetObjectItem(params, "dest");
    const cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(dest) || !cJSON_IsString(text)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    cJSON_AddStringToObject(result, "status", "sent");
    return 0;
}

static int h_set_radio(const cJSON *params, cJSON *result) {
    const cJSON *tx = cJSON_GetObjectItem(params, "tx_power");
    if (tx && !cJSON_IsNumber(tx)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    if (tx && tx->valueint > 30) {
        cJSON_AddStringToObject(result, "error", "tx power exceeds plan");
        return RPC_ERR_INVALID_PARAMS;
    }
    cJSON_AddStringToObject(result, "status", "ok");
    return 0;
}

static int h_radio_failure(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddStringToObject(result, "error", "send failed");
    return RPC_ERR_RADIO;
}

static int h_set_location_config(const cJSON *params, cJSON *result) {
    const cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    const cJSON *default_tier = cJSON_GetObjectItem(params, "default_tier");
    const cJSON *tier_alias = cJSON_GetObjectItem(params, "tier");
    const cJSON *interval_s = cJSON_GetObjectItem(params, "interval_s");
    const cJSON *source = cJSON_GetObjectItem(params, "source");
    const cJSON *contact_rules = cJSON_GetObjectItem(params, "contact_rules");
    const cJSON *channel_targets = cJSON_GetObjectItem(params, "channel_targets");

    if (enabled && !cJSON_IsBool(enabled)) return RPC_ERR_INVALID_PARAMS;
    if (default_tier && !cJSON_IsString(default_tier)) return RPC_ERR_INVALID_PARAMS;
    if (tier_alias) return RPC_ERR_INVALID_PARAMS;
    if (interval_s && !cJSON_IsNumber(interval_s)) return RPC_ERR_INVALID_PARAMS;
    if (source && !cJSON_IsString(source)) return RPC_ERR_INVALID_PARAMS;
    if (contact_rules && !cJSON_IsArray(contact_rules)) return RPC_ERR_INVALID_PARAMS;
    if (channel_targets && !cJSON_IsArray(channel_targets)) return RPC_ERR_INVALID_PARAMS;

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int h_share_location_once(const cJSON *params, cJSON *result) {
    const cJSON *address = cJSON_GetObjectItem(params, "address");
    const cJSON *tier = cJSON_GetObjectItem(params, "tier");
    if (!cJSON_IsString(address)) return RPC_ERR_INVALID_PARAMS;
    if (tier && !cJSON_IsString(tier)) return RPC_ERR_INVALID_PARAMS;

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "packetType", "PKT_TYPE_LOCATION");
    cJSON_AddStringToObject(result, "tier", tier && tier->valuestring ? tier->valuestring : "coarse");
    return 0;
}

void test_send_message_missing_params_returns_invalid_params(void) {
    rpc_register("bramble.sendMessage", h_send_message);

    char response[512];
    int len = rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.sendMessage\",\"params\":{\"dest\":\"ABCDEF01\"}}",
                           response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_radio_out_of_range_returns_invalid_params(void) {
    rpc_register("bramble.setRadio", h_set_radio);

    char response[512];
    int len = rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bramble.setRadio\",\"params\":{\"tx_power\":99}}",
                           response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_handler_radio_error_bubbles_to_error_response(void) {
    rpc_register("bramble.sendProbe", h_radio_failure);

    char response[512];
    int len = rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"bramble.sendProbe\",\"params\":{}}",
                           response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(RPC_ERR_RADIO, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_location_config_accepts_hybrid_policy_fields(void) {
    rpc_register("bramble.setLocationConfig", h_set_location_config);

    char response[1024];
    int len = rpc_dispatch(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"bramble.setLocationConfig\",\"params\":{"
        "\"enabled\":true,\"default_tier\":\"coarse\",\"interval_s\":300,\"source\":\"hybrid\","
        "\"contact_rules\":[{\"address\":\"AABBCCDD\",\"enabled\":true,\"tier\":\"full\",\"interval_s\":120}],"
        "\"channel_targets\":[{\"channel\":0,\"enabled\":true,\"tier\":\"coarse\",\"interval_s\":600}]}}",
        response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(resp, "result"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(resp, "error"));
    cJSON_Delete(resp);
}

void test_set_location_config_rejects_tier_alias(void) {
    rpc_register("bramble.setLocationConfig", h_set_location_config);

    char response[1024];
    int len = rpc_dispatch(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"bramble.setLocationConfig\",\"params\":{"
        "\"enabled\":true,\"tier\":\"coarse\"}}",
        response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_location_config_rejects_invalid_hybrid_policy_shapes(void) {
    rpc_register("bramble.setLocationConfig", h_set_location_config);

    char response[1024];
    int len = rpc_dispatch(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"bramble.setLocationConfig\",\"params\":{"
        "\"enabled\":\"yes\",\"contact_rules\":{},\"channel_targets\":\"bad\"}}",
        response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(RPC_ERR_INVALID_PARAMS, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_share_location_once_reports_location_packet_type(void) {
    rpc_register("bramble.shareLocationOnce", h_share_location_once);

    char response[512];
    int len = rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bramble.shareLocationOnce\",\"params\":{\"address\":\"AABBCCDD\"}}",
                           response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_EQUAL_STRING("PKT_TYPE_LOCATION", cJSON_GetObjectItem(result, "packetType")->valuestring);
    cJSON_Delete(resp);
}

void test_share_location_once_honors_requested_tier(void) {
    rpc_register("bramble.shareLocationOnce", h_share_location_once);

    char response[512];
    int len = rpc_dispatch("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"bramble.shareLocationOnce\",\"params\":{\"address\":\"AABBCCDD\",\"tier\":\"presence\"}}",
                           response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_EQUAL_STRING("presence", cJSON_GetObjectItem(result, "tier")->valuestring);
    cJSON_Delete(resp);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_send_message_missing_params_returns_invalid_params);
    RUN_TEST(test_set_radio_out_of_range_returns_invalid_params);
    RUN_TEST(test_handler_radio_error_bubbles_to_error_response);
    RUN_TEST(test_set_location_config_accepts_hybrid_policy_fields);
    RUN_TEST(test_set_location_config_rejects_tier_alias);
    RUN_TEST(test_set_location_config_rejects_invalid_hybrid_policy_shapes);
    RUN_TEST(test_share_location_once_reports_location_packet_type);
    RUN_TEST(test_share_location_once_honors_requested_tier);
    return UNITY_END();
}
