#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_send_message_missing_params_returns_invalid_params);
    RUN_TEST(test_set_radio_out_of_range_returns_invalid_params);
    RUN_TEST(test_handler_radio_error_bubbles_to_error_response);
    return UNITY_END();
}
