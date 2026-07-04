#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include <string.h>

extern char g_last_channel_name[64];
extern unsigned char g_last_channel_psk[128];
extern size_t g_last_channel_psk_len;
extern int g_mesh_add_channel_calls;
extern int g_mesh_add_channel_return;

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

void setUp(void) {
    rpc_init();
    rpc_methods_init(&s_id);
    g_last_channel_name[0] = '\0';
    g_last_channel_psk_len = 0;
    g_mesh_add_channel_calls = 0;
    g_mesh_add_channel_return = 2;
}

void tearDown(void) {}

void test_add_channel_forwards_name_and_passphrase_psk_to_mesh_add_channel(void) {
    char response[512];
    const char* req = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bramble.addChannel\",\"params\":{"
                      "\"name\":\"ops-room\",\"psk\":\"meshpass42\"}}";

    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL(1, g_mesh_add_channel_calls);
    TEST_ASSERT_EQUAL_STRING("ops-room", g_last_channel_name);
    TEST_ASSERT_EQUAL_UINT32(strlen("meshpass42"), g_last_channel_psk_len);
    TEST_ASSERT_EQUAL_UINT8('m', g_last_channel_psk[0]);
    TEST_ASSERT_EQUAL_UINT8('e', g_last_channel_psk[1]);

    cJSON* j = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(j);
    cJSON* res = cJSON_GetObjectItem(j, "result");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(res, "ok")));
    cJSON_Delete(j);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_channel_forwards_name_and_passphrase_psk_to_mesh_add_channel);
    return UNITY_END();
}
