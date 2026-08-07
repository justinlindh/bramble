#include "unity.h"
#include "cJSON.h"
#include "location.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include <stdint.h>
#include <string.h>

extern bool g_stub_mailbox_enabled;
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
extern int g_nvs_loc_kv_count;
extern struct {
    char key[16];
    char value[64];
    bool used;
} g_nvs_loc_kv[16];
extern int g_nvs_loc_blob_count;
extern struct {
    char key[16];
    uint8_t value[64];
    size_t len;
    bool used;
} g_nvs_loc_blob[16];

/* Controllable clock (ESP_TIMER_CUSTOM_IMPL is set in CMake): whether a stored
 * position reads as current is a question about where "now" sits relative to
 * it, which a clock frozen at zero cannot ask. */
static int64_t s_now_us = 0;
int64_t esp_timer_get_time(void) { return s_now_us; }
static void set_now_ms(uint32_t ms) { s_now_us = (int64_t)ms * 1000; }

static bramble_identity_t s_id = {
    .address = 0xAABBCCDD,
    .pubkey_hash = 0x11223344,
};

static cJSON* dispatch_request(const char* req) {
    char response[2048];
    int len = rpc_dispatch(req, response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON* root = cJSON_Parse(response);
    TEST_ASSERT_NOT_NULL(root);
    return root;
}

static cJSON* dispatch_get_config(void) {
    return dispatch_request(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"bramble.getConfig\",\"params\":{}}");
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

    g_nvs_loc_kv_count = 0;
    memset(g_nvs_loc_kv, 0, sizeof(g_nvs_loc_kv));
    g_nvs_loc_blob_count = 0;
    memset(g_nvs_loc_blob, 0, sizeof(g_nvs_loc_blob));

    location_store_reset_boot_state();
    set_now_ms(0);

    g_stub_mailbox_enabled = false;
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

    cJSON* root = dispatch_get_config();
    cJSON* result = cJSON_GetObjectItem(root, "result");
    TEST_ASSERT_NOT_NULL(result);

    cJSON* channels = cJSON_GetObjectItem(result, "channels");
    TEST_ASSERT_TRUE(cJSON_IsArray(channels));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(channels));

    cJSON* ch1 = cJSON_GetArrayItem(channels, 1);
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

    cJSON* root = dispatch_get_config();
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* channels = cJSON_GetObjectItem(result, "channels");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(channels));

    cJSON* ch0 = cJSON_GetArrayItem(channels, 0);
    TEST_ASSERT_EQUAL_STRING("Broadcast", cJSON_GetObjectItem(ch0, "name")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(ch0, "is_default")));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(ch0, "hasPsk")));

    cJSON_Delete(root);
}

void test_location_contact_roundtrip_uses_canonical_rule_key(void) {
    g_nvs_allow_open = true;

    cJSON* set_root =
        dispatch_request("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"bramble.setLocationContact\","
                         "\"params\":{\"address\":\"AABBCCDD\",\"tier\":\"full\",\"enabled\":true,"
                         "\"interval_s\":120}}");
    cJSON* set_result = cJSON_GetObjectItem(set_root, "result");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(set_result, "ok")));
    cJSON_Delete(set_root);

    bool saw_lcr = false;
    bool saw_lc = false;
    for (int i = 0; i < 16; i++) {
        if (!g_nvs_loc_kv[i].used)
            continue;
        if (strcmp(g_nvs_loc_kv[i].key, "lcr_AABBCCDD") == 0)
            saw_lcr = true;
        if (strcmp(g_nvs_loc_kv[i].key, "lc_AABBCCDD") == 0)
            saw_lc = true;
    }
    TEST_ASSERT_TRUE(saw_lcr);
    TEST_ASSERT_FALSE(saw_lc);

    cJSON* cfg_root = dispatch_get_config();
    cJSON* cfg_result = cJSON_GetObjectItem(cfg_root, "result");
    cJSON* location = cJSON_GetObjectItem(cfg_result, "location");
    cJSON* contact_rules = cJSON_GetObjectItem(location, "contact_rules");
    TEST_ASSERT_TRUE(cJSON_IsArray(contact_rules));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(contact_rules));

    cJSON* rule = cJSON_GetArrayItem(contact_rules, 0);
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", cJSON_GetObjectItem(rule, "address")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(rule, "enabled")));
    TEST_ASSERT_EQUAL_STRING("full", cJSON_GetObjectItem(rule, "tier")->valuestring);
    TEST_ASSERT_EQUAL(120, cJSON_GetObjectItem(rule, "interval_s")->valueint);

    cJSON_Delete(cfg_root);
}

void test_get_config_ignores_legacy_location_contact_keys(void) {
    g_nvs_allow_open = true;
    strcpy(g_nvs_loc_kv[0].key, "lc_DEADBEEF");
    strcpy(g_nvs_loc_kv[0].value, "full");
    g_nvs_loc_kv[0].used = true;
    g_nvs_loc_kv_count = 1;

    cJSON* cfg_root = dispatch_get_config();
    cJSON* cfg_result = cJSON_GetObjectItem(cfg_root, "result");
    cJSON* location = cJSON_GetObjectItem(cfg_result, "location");
    cJSON* contact_rules = cJSON_GetObjectItem(location, "contact_rules");
    TEST_ASSERT_TRUE(cJSON_IsArray(contact_rules));
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(contact_rules));
    cJSON_Delete(cfg_root);
}

void test_get_config_location_includes_canonical_fields_shape(void) {
    g_nvs_allow_open = false;

    cJSON* cfg_root = dispatch_get_config();
    cJSON* cfg_result = cJSON_GetObjectItem(cfg_root, "result");
    cJSON* location = cJSON_GetObjectItem(cfg_result, "location");
    TEST_ASSERT_NOT_NULL(location);

    TEST_ASSERT_TRUE(cJSON_IsBool(cJSON_GetObjectItem(location, "enabled")));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(location, "tier")));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(location, "default_tier")));
    TEST_ASSERT_TRUE(cJSON_IsNumber(cJSON_GetObjectItem(location, "interval_s")));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(location, "source")));
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItem(location, "contact_rules")));
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItem(location, "channel_targets")));

    cJSON_Delete(cfg_root);
}

/* Stage one persisted peer record. The layout comes from location.h: this
 * test used to carry its own copy of it, which is precisely how a flash layout
 * drifts from the code that reads it. Coordinates are fictional. */
static void stage_peer_record(const char* key, int32_t lat_e7, int32_t lon_e7, uint8_t tier,
                              uint32_t received_ms, uint32_t boot_id, bool legacy_layout) {
    bramble_position_t pos;
    memset(&pos, 0, sizeof(pos));
    pos.latitude_e7 = lat_e7;
    pos.longitude_e7 = lon_e7;
    pos.altitude_m = 15;
    pos.accuracy_m = 8;
    pos.speed_kmh = 12;
    pos.heading_deg2 = 45;
    pos.timestamp = 1234;
    pos.valid = true;

    persisted_peer_location_t stored;
    peer_location_record_encode(&stored, &pos, tier, received_ms, boot_id);

    size_t len = legacy_layout ? PEER_LOCATION_RECORD_V0_SIZE : sizeof(stored);
    int i = g_nvs_loc_blob_count;
    strcpy(g_nvs_loc_blob[i].key, key);
    memcpy(g_nvs_loc_blob[i].value, &stored, len);
    g_nvs_loc_blob[i].len = len;
    g_nvs_loc_blob[i].used = true;
    g_nvs_loc_blob_count = i + 1;
}

static cJSON* dispatch_get_peer_locations(void) {
    return dispatch_request(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"bramble.getPeerLocations\",\"params\":{}}");
}

void test_get_peer_locations_exports_peer_identity_and_timestamps(void) {
    g_nvs_allow_open = true;
    uint32_t boot_id = location_store_begin_boot();
    stage_peer_record("lp_A1B2C3D4", 123456700, -456789000, LOCATION_TIER_FULL, 4242, boot_id,
                      false);
    set_now_ms(9000); /* received during this boot, well inside the TTL */

    cJSON* root = dispatch_get_peer_locations();
    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* peer_locations = cJSON_GetObjectItem(result, "peerLocations");
    TEST_ASSERT_TRUE(cJSON_IsArray(peer_locations));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(peer_locations));

    cJSON* peer = cJSON_GetArrayItem(peer_locations, 0);
    cJSON* addr = cJSON_GetObjectItem(peer, "addr");
    TEST_ASSERT_TRUE(cJSON_IsString(addr));
    TEST_ASSERT_EQUAL_STRING("A1B2C3D4", addr->valuestring);
    TEST_ASSERT_EQUAL_STRING("full", cJSON_GetObjectItem(peer, "tier")->valuestring);
    TEST_ASSERT_EQUAL(4242, cJSON_GetObjectItem(peer, "lastUpdatedMs")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(peer, "online")));

    cJSON* legacy = cJSON_GetObjectItem(result, "peers");
    TEST_ASSERT_NULL(legacy);

    cJSON_Delete(root);
}

void test_get_peer_locations_reports_a_previous_boots_record_as_offline(void) {
    /* The bench symptom: a stored uptime LARGER than the node's current one,
     * reported as a fresh position because the arithmetic clamped to zero.
     * A record from another boot has no computable age, so it is never
     * "online" and its lastUpdatedMs is 0 rather than a plausible-looking
     * value on a clock that never produced it. */
    g_nvs_allow_open = true;
    uint32_t boot_id = location_store_begin_boot();
    stage_peer_record("lp_D0C9D311", 123456700, -456789000, LOCATION_TIER_FULL, 3601167,
                      boot_id - 1, false);
    set_now_ms(900000); /* up ~15 minutes, against a stored 3601167 */

    cJSON* root = dispatch_get_peer_locations();
    cJSON* peer_locations =
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "result"), "peerLocations");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(peer_locations));

    cJSON* peer = cJSON_GetArrayItem(peer_locations, 0);
    /* The position is still exported: unknown age, known place. */
    cJSON* position = cJSON_GetObjectItem(peer, "position");
    double lat = cJSON_GetObjectItem(position, "lat")->valuedouble;
    TEST_ASSERT_TRUE(lat > 12.3 && lat < 12.4);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(peer, "online")));
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(peer, "lastUpdatedMs")->valueint);

    cJSON_Delete(root);
}

void test_get_peer_locations_reads_records_written_before_the_boot_counter(void) {
    /* Records in the shorter pre-boot_id layout are on flash on every node
     * that has run an earlier build. They must still export, as age-unknown. */
    g_nvs_allow_open = true;
    location_store_begin_boot();
    stage_peer_record("lp_3575D5D7", 123456700, -456789000, LOCATION_TIER_COARSE, 5000, 0, true);

    cJSON* root = dispatch_get_peer_locations();
    cJSON* peer_locations =
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "result"), "peerLocations");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(peer_locations));

    cJSON* peer = cJSON_GetArrayItem(peer_locations, 0);
    TEST_ASSERT_EQUAL_STRING("3575D5D7", cJSON_GetObjectItem(peer, "addr")->valuestring);
    TEST_ASSERT_EQUAL_STRING("coarse", cJSON_GetObjectItem(peer, "tier")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(peer, "online")));
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(peer, "lastUpdatedMs")->valueint);

    cJSON_Delete(root);
}

void test_get_config_returns_mailbox_enabled_false_by_default(void) {
    g_stub_mailbox_enabled = false;

    cJSON* root = dispatch_get_config();
    cJSON* result = cJSON_GetObjectItem(root, "result");
    TEST_ASSERT_NOT_NULL(result);

    cJSON* mailbox_enabled = cJSON_GetObjectItem(result, "mailboxEnabled");
    TEST_ASSERT_NOT_NULL_MESSAGE(mailbox_enabled,
                                 "mailboxEnabled field must be present in getConfig response");
    TEST_ASSERT_TRUE(cJSON_IsBool(mailbox_enabled));
    TEST_ASSERT_FALSE(cJSON_IsTrue(mailbox_enabled));

    cJSON_Delete(root);
}

void test_get_config_returns_mailbox_enabled_true_when_set(void) {
    g_stub_mailbox_enabled = true;

    cJSON* root = dispatch_get_config();
    cJSON* result = cJSON_GetObjectItem(root, "result");
    TEST_ASSERT_NOT_NULL(result);

    cJSON* mailbox_enabled = cJSON_GetObjectItem(result, "mailboxEnabled");
    TEST_ASSERT_NOT_NULL_MESSAGE(mailbox_enabled,
                                 "mailboxEnabled field must be present in getConfig response");
    TEST_ASSERT_TRUE(cJSON_IsBool(mailbox_enabled));
    TEST_ASSERT_TRUE(cJSON_IsTrue(mailbox_enabled));

    cJSON_Delete(root);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_config_uses_persisted_name_and_psk_when_runtime_cache_missing);
    RUN_TEST(test_get_config_keeps_default_broadcast_semantics);
    RUN_TEST(test_location_contact_roundtrip_uses_canonical_rule_key);
    RUN_TEST(test_get_config_ignores_legacy_location_contact_keys);
    RUN_TEST(test_get_config_location_includes_canonical_fields_shape);
    RUN_TEST(test_get_peer_locations_exports_peer_identity_and_timestamps);
    RUN_TEST(test_get_peer_locations_reports_a_previous_boots_record_as_offline);
    RUN_TEST(test_get_peer_locations_reads_records_written_before_the_boot_counter);
    RUN_TEST(test_get_config_returns_mailbox_enabled_false_by_default);
    RUN_TEST(test_get_config_returns_mailbox_enabled_true_when_set);
    return UNITY_END();
}
