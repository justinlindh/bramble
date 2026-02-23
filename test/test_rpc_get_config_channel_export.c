#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include <string.h>

extern int g_mesh_channel_count;
extern int g_mesh_default_channel;
extern char g_mesh_channel_names[8][20];
extern bool g_mesh_channel_has_psk[8];
extern uint16_t g_mesh_channel_epoch[8];

extern bool g_nvs_allow_open;
extern char g_nvs_node_name[64];
extern char g_nvs_channel_names[8][20];
extern uint8_t g_nvs_channel_psk_flags[8];
extern bool g_nvs_channel_psk_present[8];

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

static cJSON *dispatch_get_config(void) {
    char response[2048];
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"bramble.getConfig\",\"params\":{}}";
    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON *root = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(root);
    return root;
}

void setUp(void) {
    rpc_init();
    rpc_methods_init(&s_id);

    g_mesh_channel_count = 1;
    g_mesh_default_channel = 0;
    memset(g_mesh_channel_names, 0, sizeof(g_mesh_channel_names));
    strcpy(g_mesh_channel_names[0], "Broadcast");
    memset(g_mesh_channel_has_psk, 0, sizeof(g_mesh_channel_has_psk));
    memset(g_mesh_channel_epoch, 0, sizeof(g_mesh_channel_epoch));

    g_nvs_allow_open = false;
    memset(g_nvs_node_name, 0, sizeof(g_nvs_node_name));
    memset(g_nvs_channel_names, 0, sizeof(g_nvs_channel_names));
    memset(g_nvs_channel_psk_flags, 0, sizeof(g_nvs_channel_psk_flags));
    memset(g_nvs_channel_psk_present, 0, sizeof(g_nvs_channel_psk_present));
}

void tearDown(void) {}

void test_get_config_uses_persisted_name_and_psk_when_runtime_cache_missing(void) {
    g_mesh_channel_count = 2;
    g_mesh_default_channel = 1;
    strcpy(g_mesh_channel_names[0], "Broadcast");
    g_mesh_channel_names[1][0] = '\0';
    g_mesh_channel_has_psk[1] = false;
    g_mesh_channel_epoch[1] = 7;

    g_nvs_allow_open = true;
    strcpy(g_nvs_channel_names[1], "ops-room");
    g_nvs_channel_psk_present[1] = true;
    g_nvs_channel_psk_flags[1] = 1;

    cJSON *root = dispatch_get_config();
    cJSON *result = cJSON_GetObjectItem(root, "result");
    TEST_ASSERT_NOT_NULL(result);

    cJSON *channels = cJSON_GetObjectItem(result, "channels");
    TEST_ASSERT_TRUE(cJSON_IsArray(channels));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(channels));

    cJSON *ch1 = cJSON_GetArrayItem(channels, 1);
    TEST_ASSERT_EQUAL_STRING("ops-room", cJSON_GetObjectItem(ch1, "name")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(ch1, "hasPsk")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(ch1, "is_default")));

    cJSON_Delete(root);
}

void test_get_config_keeps_default_broadcast_semantics(void) {
    g_mesh_channel_count = 1;
    g_mesh_default_channel = 0;
    strcpy(g_mesh_channel_names[0], "Broadcast");
    g_mesh_channel_has_psk[0] = false;

    cJSON *root = dispatch_get_config();
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *channels = cJSON_GetObjectItem(result, "channels");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(channels));

    cJSON *ch0 = cJSON_GetArrayItem(channels, 0);
    TEST_ASSERT_EQUAL_STRING("Broadcast", cJSON_GetObjectItem(ch0, "name")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(ch0, "is_default")));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(ch0, "hasPsk")));

    cJSON_Delete(root);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_config_uses_persisted_name_and_psk_when_runtime_cache_missing);
    RUN_TEST(test_get_config_keeps_default_broadcast_semantics);
    return UNITY_END();
}
