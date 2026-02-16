#include "unity.h"
#include "../components/ble/json_rpc.c"

void setUp(void) {}
void tearDown(void) {}

void test_parse_simple_request(void) {
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"get_config\",\"id\":1}";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(req.valid);
    TEST_ASSERT_EQUAL(RPC_METHOD_GET_CONFIG, req.method);
    TEST_ASSERT_EQUAL_STRING("get_config", req.method_str);
    TEST_ASSERT_EQUAL(1, req.id);
    TEST_ASSERT_EQUAL(0, req.param_count);
}

void test_parse_with_params(void) {
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"set_config\",\"params\":{\"channel\":\"5\",\"power\":\"20\"},\"id\":3}";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(req.valid);
    TEST_ASSERT_EQUAL(RPC_METHOD_SET_CONFIG, req.method);
    TEST_ASSERT_EQUAL(3, req.id);
    TEST_ASSERT_EQUAL(2, req.param_count);
    TEST_ASSERT_EQUAL_STRING("channel", req.params[0].key);
    TEST_ASSERT_EQUAL_STRING("5", req.params[0].value);
    TEST_ASSERT_EQUAL_STRING("power", req.params[1].key);
    TEST_ASSERT_EQUAL_STRING("20", req.params[1].value);
}

void test_parse_send_message(void) {
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"send_message\",\"params\":{\"dest\":\"AABBCCDD\",\"text\":\"hello\"},\"id\":2}";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(req.valid);
    TEST_ASSERT_EQUAL(RPC_METHOD_SEND_MESSAGE, req.method);
    TEST_ASSERT_EQUAL(2, req.id);
    TEST_ASSERT_EQUAL(2, req.param_count);
    TEST_ASSERT_EQUAL_STRING("dest", req.params[0].key);
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", req.params[0].value);
    TEST_ASSERT_EQUAL_STRING("text", req.params[1].key);
    TEST_ASSERT_EQUAL_STRING("hello", req.params[1].value);
}

void test_invalid_json(void) {
    const char *json = "not json at all";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_NOT_EQUAL(0, ret);
    TEST_ASSERT_FALSE(req.valid);
}

void test_missing_method(void) {
    const char *json = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_NOT_EQUAL(0, ret);
    TEST_ASSERT_FALSE(req.valid);
}

void test_build_success_response(void) {
    rpc_response_t resp = { .id = 1, .is_error = false };
    snprintf(resp.result, sizeof(resp.result), "\"ok\"");
    char buf[256];
    rpc_build_response(&resp, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"jsonrpc\":\"2.0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"result\":\"ok\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":1"));
}

void test_build_error_response(void) {
    char buf[256];
    rpc_build_error(5, -32600, "Invalid Request", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"error\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "-32600"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Invalid Request"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":5"));
}

void test_method_lookup(void) {
    TEST_ASSERT_EQUAL(RPC_METHOD_GET_CONFIG, rpc_method_from_string("get_config"));
    TEST_ASSERT_EQUAL(RPC_METHOD_SET_CONFIG, rpc_method_from_string("set_config"));
    TEST_ASSERT_EQUAL(RPC_METHOD_SEND_MESSAGE, rpc_method_from_string("send_message"));
    TEST_ASSERT_EQUAL(RPC_METHOD_GET_NODES, rpc_method_from_string("get_nodes"));
    TEST_ASSERT_EQUAL(RPC_METHOD_GET_ROUTES, rpc_method_from_string("get_routes"));
    TEST_ASSERT_EQUAL(RPC_METHOD_GET_STATUS, rpc_method_from_string("get_status"));
    TEST_ASSERT_EQUAL(RPC_METHOD_GET_MESSAGES, rpc_method_from_string("get_messages"));
    TEST_ASSERT_EQUAL(RPC_METHOD_UNKNOWN, rpc_method_from_string("bogus"));
}

void test_truncated_input(void) {
    const char *json = "{\"jsonrpc\":\"2.0\",\"meth";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_NOT_EQUAL(0, ret);
    TEST_ASSERT_FALSE(req.valid);
}

void test_null_input(void) {
    rpc_request_t req;
    int ret = rpc_parse_request(NULL, 0, &req);
    TEST_ASSERT_NOT_EQUAL(0, ret);
    TEST_ASSERT_FALSE(req.valid);
}

void test_empty_object(void) {
    const char *json = "{}";
    rpc_request_t req;
    int ret = rpc_parse_request(json, strlen(json), &req);
    TEST_ASSERT_NOT_EQUAL(0, ret);
    TEST_ASSERT_FALSE(req.valid);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_simple_request);
    RUN_TEST(test_parse_with_params);
    RUN_TEST(test_parse_send_message);
    RUN_TEST(test_invalid_json);
    RUN_TEST(test_missing_method);
    RUN_TEST(test_build_success_response);
    RUN_TEST(test_build_error_response);
    RUN_TEST(test_method_lookup);
    RUN_TEST(test_truncated_input);
    RUN_TEST(test_null_input);
    RUN_TEST(test_empty_object);
    return UNITY_END();
}
