#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

void setUp(void) { rpc_init(); }
void tearDown(void) {}

static int mock_handler(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddStringToObject(result, "hello", "world");
    return 0;
}

static int mock_handler_with_params(const cJSON *params, cJSON *result) {
    cJSON *val = cJSON_GetObjectItem(params, "name");
    if (val && cJSON_IsString(val)) {
        cJSON_AddStringToObject(result, "greeting", val->valuestring);
    } else {
        cJSON_AddStringToObject(result, "greeting", "anonymous");
    }
    return 0;
}

static int mock_handler_error(const cJSON *params, cJSON *result) {
    (void)params;
    (void)result;
    return -1001;
}

/* Helper: parse response and check jsonrpc field */
static cJSON *parse_response(const char *resp) {
    cJSON *j = cJSON_Parse(resp);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("2.0", cJSON_GetObjectItem(j, "jsonrpc")->valuestring);
    return j;
}

void test_dispatch_valid_request(void) {
    rpc_register("test.method", mock_handler);

    char response[1024];
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test.method\"}";
    int len = rpc_dispatch(request, response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    TEST_ASSERT_EQUAL(1, cJSON_GetObjectItem(resp, "id")->valueint);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("world", cJSON_GetObjectItem(result, "hello")->valuestring);
    cJSON_Delete(resp);
}

void test_dispatch_unknown_method(void) {
    char response[1024];
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"no.such.method\"}";
    int len = rpc_dispatch(request, response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL(-32601, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_dispatch_malformed_json(void) {
    char response[1024];
    int len = rpc_dispatch("{not valid json", response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL(-32700, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_dispatch_missing_jsonrpc_field(void) {
    char response[1024];
    const char *request = "{\"id\":1,\"method\":\"test\"}";
    int len = rpc_dispatch(request, response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL(-32600, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_dispatch_missing_method(void) {
    char response[1024];
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    int len = rpc_dispatch(request, response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL(-32600, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_dispatch_no_params(void) {
    rpc_register("greet", mock_handler_with_params);

    char response[1024];
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"greet\"}";
    int len = rpc_dispatch(request, response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("anonymous", cJSON_GetObjectItem(result, "greeting")->valuestring);
    cJSON_Delete(resp);
}

void test_register_max_methods(void) {
    char name[32];
    for (int i = 0; i < CONFIG_BRAMBLE_RPC_MAX_METHODS; i++) {
        snprintf(name, sizeof(name), "method.%d", i);
        TEST_ASSERT_EQUAL(0, rpc_register(name, mock_handler));
    }
    TEST_ASSERT_EQUAL(-1, rpc_register("overflow", mock_handler));
}

void test_handler_error_code(void) {
    rpc_register("fail", mock_handler_error);

    char response[1024];
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"fail\"}";
    int len = rpc_dispatch(request, response, sizeof(response));

    TEST_ASSERT_GREATER_THAN(0, len);

    cJSON *resp = parse_response(response);
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL(-1001, cJSON_GetObjectItem(err, "code")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(resp, "result"));
    cJSON_Delete(resp);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dispatch_valid_request);
    RUN_TEST(test_dispatch_unknown_method);
    RUN_TEST(test_dispatch_malformed_json);
    RUN_TEST(test_dispatch_missing_jsonrpc_field);
    RUN_TEST(test_dispatch_missing_method);
    RUN_TEST(test_dispatch_no_params);
    RUN_TEST(test_register_max_methods);
    RUN_TEST(test_handler_error_code);
    return UNITY_END();
}
