/**
 * test_rpc_methods_firmware.c
 *
 * Tests against the real rpc_methods.c handlers (not mock handlers).
 * Covers: getStatus, getNeighbors, exportTopology, sendMessage, sendBroadcast,
 *         otaUpdate, setNodeName.
 */
#include "unity.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "phy_passthrough.h"
#include "ota_progress.h"
#include "mesh_task.h"
#include "routing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Controllable stubs from rpc_methods_test_stubs.c */
extern uint32_t g_stub_send_message_return;
extern uint32_t g_stub_send_channel_return;
extern int g_stub_send_broadcast_return;
extern uint32_t g_stub_last_broadcast_id;
extern bool g_nvs_allow_open;
extern char g_nvs_node_name[64];
extern uint8_t g_nvs_gps_en;
extern bool g_stub_board_has_gps;
extern bool g_nvs_lat_e6_set;
extern int32_t g_nvs_lat_e6;
extern bool g_nvs_lon_e6_set;
extern int32_t g_nvs_lon_e6;
extern int g_wifi_set_creds_rc;
extern char g_wifi_set_creds_ssid[33];
extern char g_wifi_set_creds_password[65];

/* Observed-state hooks: what mesh_get_state / mesh_get_routes / radio_get_config
 * hand the handlers, so bramble.exportTopology can be checked against known
 * neighbours, routes and a known PHY rather than against an empty mesh. */
extern void (*g_stub_mesh_state_fill)(mesh_shared_state_t* out);
extern void (*g_stub_mesh_routes_fill)(routing_table_t* out);
void stub_set_radio_config(float frequency_mhz, uint8_t sf, uint32_t bw_hz, uint8_t coding_rate,
                           int8_t tx_power);

/* phy.tx routes raw frames through the tx gate; the stub captures the call. */
extern int g_stub_tx_gate_calls;
extern uint8_t g_stub_tx_gate_last_frame[255];
extern uint8_t g_stub_tx_gate_last_len;

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
    g_nvs_gps_en = 1;
    g_stub_board_has_gps = false;
    g_nvs_lat_e6_set = false;
    g_nvs_lat_e6 = 0;
    g_nvs_lon_e6_set = false;
    g_nvs_lon_e6 = 0;
    g_wifi_set_creds_rc = 0;
    g_wifi_set_creds_ssid[0] = '\0';
    g_wifi_set_creds_password[0] = '\0';
    /* PHY passthrough is module-global state that persists across tests; force
     * every case to start from a disabled gate and a clean tx-gate capture. */
    phy_passthrough_disable();
    g_stub_tx_gate_calls = 0;
    g_stub_tx_gate_last_len = 0;
    /* Observed-state hooks are process-global too: every case starts from an
     * empty mesh and a zeroed PHY unless it plants its own. */
    g_stub_mesh_state_fill = NULL;
    g_stub_mesh_routes_fill = NULL;
    stub_set_radio_config(0.0f, 0, 0, 0, 0);
}

void tearDown(void) {}

/* Suppress known OTA strdup leak in test environment */
const char* __asan_default_options(void) { return "detect_leaks=0"; }

/* Strong override of rpc_methods.c's weak bramble_platform_enter_dfu so both
 * enterDfu paths are testable from one binary: the weak default (no DFU
 * bootloader, the ESP case) and a platform that accepts (the nRF UF2 case). */
static int s_enter_dfu_rc = -1;
static int s_enter_dfu_calls = 0;
int bramble_platform_enter_dfu(void) {
    s_enter_dfu_calls++;
    return s_enter_dfu_rc;
}

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

    /* Charging-aware additive fields (stub reports UNKNOWN/present). */
    cJSON* charging = cJSON_GetObjectItem(r, "charging");
    TEST_ASSERT_NOT_NULL(charging);
    TEST_ASSERT_EQUAL_STRING("unknown", charging->valuestring);
    cJSON* present = cJSON_GetObjectItem(r, "present");
    TEST_ASSERT_NOT_NULL(present);
    TEST_ASSERT_TRUE(cJSON_IsTrue(present));

    /* Per-node identity Phase 4 diagnostics (additive fields). */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "identity_pins"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "identity_conflicts"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "identity_sig_failures"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "identity_addr_mismatches"));

    /* gps_enabled mirrors the persisted preference regardless of gps_available. */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_enabled"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "gps_enabled")));

    cJSON_Delete(resp);
}

/* ── 1a. getBattery ───────────────────────────────────────────────── */

void test_get_battery_returns_charging_and_present_fields(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bramble.getBattery\",\"params\":{}}");
    cJSON* r = get_result(resp);

    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "voltage_mv"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "percentage"));

    cJSON* charging = cJSON_GetObjectItem(r, "charging");
    TEST_ASSERT_NOT_NULL(charging);
    TEST_ASSERT_EQUAL_STRING("unknown", charging->valuestring);
    cJSON* present = cJSON_GetObjectItem(r, "present");
    TEST_ASSERT_NOT_NULL(present);
    TEST_ASSERT_TRUE(cJSON_IsTrue(present));

    cJSON_Delete(resp);
}

/* ── 1b. getStatus GNSS observability fields ──────────────────────── */

/* The six fields are always emitted so their absence means exactly one thing:
 * firmware that predates them. A client can then render "unknown" instead of
 * reporting zero satellites, which would name the wrong failure class. */
static void assert_gnss_status_fields_present(cJSON* r) {
    cJSON* state = cJSON_GetObjectItem(r, "gps_state");
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_TRUE(cJSON_IsString(state));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_sats_in_view"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_sats_tracked"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_sats_used"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_snr_max_dbhz"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_fix_quality"));
}

void test_get_status_includes_gnss_fields(void) {
    g_stub_board_has_gps = true;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":102,\"method\":\"bramble.getStatus\",\"params\":{}}");
    cJSON* r = get_result(resp);

    assert_gnss_status_fields_present(r);
    /* A capable, powered receiver that has sent nothing at all is the severe
     * case and must be named as such: not "absent" (which claims the board has
     * no receiver) and not "acquiring" (which claims something is being
     * heard). The harness GPS backend reports a feed that has never produced
     * an NMEA line. */
    TEST_ASSERT_EQUAL_STRING("no_signal", cJSON_GetObjectItem(r, "gps_state")->valuestring);

    cJSON_Delete(resp);
}

/* The mirror image of test_get_diagnostics_omits_gps_fields_without_gps_cap:
 * getStatus keeps the fields so a client disambiguates on gps_available, never
 * on a satellite count. */
void test_get_status_gnss_fields_present_without_gps_cap(void) {
    g_stub_board_has_gps = false;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":103,\"method\":\"bramble.getStatus\",\"params\":{}}");
    cJSON* r = get_result(resp);

    assert_gnss_status_fields_present(r);
    TEST_ASSERT_EQUAL_STRING("absent", cJSON_GetObjectItem(r, "gps_state")->valuestring);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(r, "gps_sats_in_view")->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(r, "gps_sats_tracked")->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(r, "gps_sats_used")->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(r, "gps_snr_max_dbhz")->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(r, "gps_fix_quality")->valueint);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(r, "gps_available")));

    cJSON_Delete(resp);
}

/* ── 1c. getGpsPosition ───────────────────────────────────────────── */

/* The direct regression guard for the field failure: a node that never
 * acquires a fix answered with nothing but valid:false, which cannot tell a
 * dead antenna from a cold start. */
void test_get_gps_position_includes_gnss_fields_without_fix(void) {
    g_stub_board_has_gps = true;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":104,\"method\":\"bramble.getGpsPosition\",\"params\":{}}");
    cJSON* r = get_result(resp);

    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(r, "valid")));
    cJSON* state = cJSON_GetObjectItem(r, "state");
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_TRUE(cJSON_IsString(state));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "sats_in_view"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "sats_tracked"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "sats_used"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "snr_max_dbhz"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "fix_quality"));

    cJSON_Delete(resp);
}

void test_get_gps_position_not_supported_without_gps_cap(void) {
    g_stub_board_has_gps = false;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":105,\"method\":\"bramble.getGpsPosition\",\"params\":{}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-1004, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

/* ── 1d. getDiagnostics GPS fields ────────────────────────────────── */

void test_get_diagnostics_includes_gps_fields_when_gps_capable(void) {
    g_stub_board_has_gps = true;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"bramble.getDiagnostics\",\"params\":{}}");
    cJSON* r = get_result(resp);

    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_rx_bytes"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_rx_lines"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_chip"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_rx_overruns"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_rx_errors"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_rx_disabled"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(r, "gps_rx_rearm_fail"));

    cJSON_Delete(resp);
}

void test_get_diagnostics_omits_gps_fields_without_gps_cap(void) {
    g_stub_board_has_gps = false;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"bramble.getDiagnostics\",\"params\":{}}");
    cJSON* r = get_result(resp);

    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_rx_bytes"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_rx_lines"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_chip"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_rx_overruns"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_rx_errors"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_rx_disabled"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "gps_rx_rearm_fail"));

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

/* ── 2a. exportTopology ───────────────────────────────────────────── */

/* One neighbour and one route, planted through the stubs, so the export is
 * checked against known observations rather than against an empty mesh. */
static void export_fill_state(mesh_shared_state_t* out) {
    out->neighbors.count = 2;
    out->neighbors.entries[0].addr = 0x11112222;
    out->neighbors.entries[0].rssi = -97;
    out->neighbors.entries[0].snr = 6;
    out->neighbors.entries[0].delivery_rate = 240;
    out->neighbors.entries[0].airtime_remaining = 71;
    out->neighbors.entries[0].last_heard = 0;
    snprintf(out->neighbors.entries[0].name, sizeof(out->neighbors.entries[0].name), "ridge");
    /* A zeroed slot: the emitter drops address 0 rather than exporting a link
     * to nowhere, so the twin importer never sees a phantom node. */
    out->neighbors.entries[1].addr = 0;
}

static void export_fill_routes(routing_table_t* out) {
    out->count = 1;
    out->entries[0].dest_addr = 0x33334444;
    out->entries[0].next_hop = 0x11112222;
    out->entries[0].hop_count = 2;
    out->entries[0].metric = 180;
    out->entries[0].state = ROUTE_ACTIVE;
    out->entries[0].use_count = 5;
}

void test_export_topology_carries_identity_radio_neighbors_and_routes(void) {
    g_stub_mesh_state_fill = export_fill_state;
    g_stub_mesh_routes_fill = export_fill_routes;
    stub_set_radio_config(915.0f, 9, 125000, 1, 22);

    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"bramble.exportTopology\",\"params\":{}}");
    cJSON* r = get_result(resp);

    TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItem(r, "twin_schema")->valueint);

    cJSON* node = cJSON_GetObjectItem(r, "node");
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_EQUAL_STRING("AABBCCDD", cJSON_GetObjectItem(node, "address")->valuestring);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(node, "firmware_version"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(node, "protocol_version"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(node, "hardware"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(node, "uptime_s"));
    /* No name written to NVS in this case, so the field is absent rather than
     * carrying a placeholder the importer would have to know to ignore. */
    TEST_ASSERT_NULL(cJSON_GetObjectItem(node, "name"));

    cJSON* radio = cJSON_GetObjectItem(r, "radio");
    TEST_ASSERT_NOT_NULL(radio);
    TEST_ASSERT_EQUAL_INT(9, cJSON_GetObjectItem(radio, "sf")->valueint);
    TEST_ASSERT_EQUAL_INT(125000, cJSON_GetObjectItem(radio, "bw_hz")->valueint);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItem(radio, "coding_rate")->valueint);
    TEST_ASSERT_EQUAL_INT(22, cJSON_GetObjectItem(radio, "tx_power_dbm")->valueint);
    TEST_ASSERT_EQUAL_FLOAT(915.0, cJSON_GetObjectItem(radio, "frequency_mhz")->valuedouble);
    /* Real freq_plan table, not a stub: the region decides the duty-cycle
     * ceiling the twin is allowed to spend airtime against. */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "region"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "regulatory"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "max_duty_cycle_pct"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(radio, "duty_cycle_enforced"));

    cJSON* neighbors = cJSON_GetObjectItem(r, "neighbors");
    TEST_ASSERT_TRUE(cJSON_IsArray(neighbors));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(neighbors));
    cJSON* n0 = cJSON_GetArrayItem(neighbors, 0);
    TEST_ASSERT_EQUAL_STRING("11112222", cJSON_GetObjectItem(n0, "address")->valuestring);
    TEST_ASSERT_EQUAL_INT(-97, cJSON_GetObjectItem(n0, "rssi")->valueint);
    TEST_ASSERT_EQUAL_INT(6, cJSON_GetObjectItem(n0, "snr")->valueint);
    TEST_ASSERT_EQUAL_INT(240, cJSON_GetObjectItem(n0, "deliveryRate")->valueint);
    TEST_ASSERT_EQUAL_INT(71, cJSON_GetObjectItem(n0, "airtimeRemaining")->valueint);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(n0, "last_seen_ms"));
    TEST_ASSERT_EQUAL_STRING("ridge", cJSON_GetObjectItem(n0, "name")->valuestring);

    cJSON* routes = cJSON_GetObjectItem(r, "routes");
    TEST_ASSERT_TRUE(cJSON_IsArray(routes));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(routes));
    cJSON* r0 = cJSON_GetArrayItem(routes, 0);
    TEST_ASSERT_EQUAL_STRING("33334444", cJSON_GetObjectItem(r0, "dest")->valuestring);
    TEST_ASSERT_EQUAL_STRING("11112222", cJSON_GetObjectItem(r0, "next_hop")->valuestring);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(r0, "hop_count")->valueint);
    TEST_ASSERT_EQUAL_INT(180, cJSON_GetObjectItem(r0, "metric")->valueint);
    TEST_ASSERT_EQUAL_STRING("active", cJSON_GetObjectItem(r0, "state")->valuestring);
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetObjectItem(r0, "use_count")->valueint);

    cJSON_Delete(resp);
}

/* The export must not be a second, drifting spelling of the two methods it
 * subsumes: the arrays it embeds are compared field for field against what
 * bramble.getNeighbors and bramble.getRoutes return from the same state. */
void test_export_topology_matches_get_neighbors_and_get_routes(void) {
    g_stub_mesh_state_fill = export_fill_state;
    g_stub_mesh_routes_fill = export_fill_routes;

    cJSON* exp_resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"bramble.exportTopology\",\"params\":{}}");
    cJSON* nb_resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"bramble.getNeighbors\",\"params\":{}}");
    cJSON* rt_resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"bramble.getRoutes\",\"params\":{}}");

    char* exp_nb = cJSON_PrintUnformatted(cJSON_GetObjectItem(get_result(exp_resp), "neighbors"));
    char* got_nb = cJSON_PrintUnformatted(cJSON_GetObjectItem(get_result(nb_resp), "neighbors"));
    char* exp_rt = cJSON_PrintUnformatted(cJSON_GetObjectItem(get_result(exp_resp), "routes"));
    char* got_rt = cJSON_PrintUnformatted(cJSON_GetObjectItem(get_result(rt_resp), "routes"));

    TEST_ASSERT_EQUAL_STRING(got_nb, exp_nb);
    TEST_ASSERT_EQUAL_STRING(got_rt, exp_rt);

    free(exp_nb);
    free(got_nb);
    free(exp_rt);
    free(got_rt);
    cJSON_Delete(exp_resp);
    cJSON_Delete(nb_resp);
    cJSON_Delete(rt_resp);
}

void test_export_topology_reports_configured_node_name(void) {
    snprintf(g_nvs_node_name, sizeof(g_nvs_node_name), "ridge-relay");
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"bramble.exportTopology\",\"params\":{}}");
    cJSON* node = cJSON_GetObjectItem(get_result(resp), "node");
    TEST_ASSERT_EQUAL_STRING("ridge-relay", cJSON_GetObjectItem(node, "name")->valuestring);
    cJSON_Delete(resp);
}

/* ── 2b. setGpsEnabled ────────────────────────────────────────────── */

void test_set_gps_enabled_not_supported_without_gps_cap(void) {
    g_stub_board_has_gps = false;
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"bramble."
                                     "setGpsEnabled\",\"params\":{\"enabled\":false}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-1004, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_gps_enabled_missing_param_invalid(void) {
    g_stub_board_has_gps = true;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"bramble.setGpsEnabled\",\"params\":{}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_gps_enabled_non_bool_param_invalid(void) {
    g_stub_board_has_gps = true;
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"bramble."
                                     "setGpsEnabled\",\"params\":{\"enabled\":\"yes\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_gps_enabled_persists_and_reads_back_false(void) {
    g_stub_board_has_gps = true;
    g_nvs_gps_en = 1; /* starts ON */
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"bramble."
                                     "setGpsEnabled\",\"params\":{\"enabled\":false}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(r, "enabled")));
    /* The stub's NVS-backed store now reflects the persisted preference,
     * exactly what gps_pref_get() reads through nvs_get_u8(). */
    TEST_ASSERT_EQUAL_UINT8(0, g_nvs_gps_en);
    cJSON_Delete(resp);
}

void test_set_gps_enabled_persists_and_reads_back_true(void) {
    g_stub_board_has_gps = true;
    g_nvs_gps_en = 0; /* starts OFF */
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"bramble."
                                     "setGpsEnabled\",\"params\":{\"enabled\":true}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "enabled")));
    TEST_ASSERT_EQUAL_UINT8(1, g_nvs_gps_en);

    /* A following getStatus reflects the freshly-persisted preference. */
    cJSON* status_resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"bramble.getStatus\",\"params\":{}}");
    cJSON* status_r = get_result(status_resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(status_r, "gps_enabled")));
    cJSON_Delete(status_resp);

    cJSON_Delete(resp);
}

/* ── 2c. shareLocationOnce ────────────────────────────────────────── */

void test_share_location_once_no_source_errors(void) {
    /* No GPS fix (host build always reports none) and no manual NVS
     * location set: the resolver has no source at all. */
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"bramble."
                                     "shareLocationOnce\",\"params\":{\"address\":\"0000ABCD\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("no location available (no GPS fix and no manual location set)",
                             cJSON_GetObjectItem(r, "error")->valuestring);
    cJSON_Delete(resp);
}

void test_share_location_once_manual_nvs_succeeds(void) {
    /* setLocationConfig with lat/lon persists the manual fallback that
     * mesh_resolve_self_position reads once GPS reports no fix. */
    cJSON* set_resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"bramble.setLocationConfig\","
        "\"params\":{\"lat\":37.7749,\"lon\":-122.4194}}");
    cJSON_Delete(set_resp);

    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"bramble."
                                     "shareLocationOnce\",\"params\":{\"address\":\"0000ABCD\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_FLOAT_WITHIN(0.00001, 37.7749, cJSON_GetObjectItem(r, "lat")->valuedouble);
    TEST_ASSERT_FLOAT_WITHIN(0.00001, -122.4194, cJSON_GetObjectItem(r, "lon")->valuedouble);
    /* The stub's mesh_send_location_packet always returns 0xABCDEF01. */
    TEST_ASSERT_EQUAL_STRING("ABCDEF01", cJSON_GetObjectItem(r, "packetId")->valuestring);
    cJSON_Delete(resp);
}

/* ── 2c-bis. setLocationConfig channel targets ────────────────────── */

/* The public channel's PSK is well known, so a location target on it would
 * broadcast exact coordinates that anyone in radio range decrypts, and the
 * shared replay window is deliberately skipped there. The setter must refuse
 * it outright rather than store a rule the send path would honour. */
void test_set_location_config_rejects_public_channel_target(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"bramble.setLocationConfig\","
        "\"params\":{\"enabled\":true,\"channel_targets\":[{\"channel\":0}]}}");
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(resp, "error"));
    cJSON_Delete(resp);

    /* Nothing was stored: the location config still reports no channel target. */
    cJSON* cfg = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"bramble."
                                    "getConfig\",\"params\":{}}");
    cJSON* loc = cJSON_GetObjectItem(get_result(cfg), "location");
    cJSON* targets = cJSON_GetObjectItem(loc, "channel_targets");
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(targets));
    cJSON_Delete(cfg);
}

/* All-or-nothing: a request mixing a legal target with the public channel must
 * apply neither, so a caller cannot smuggle the public target in behind a
 * valid one. */
void test_set_location_config_public_channel_rejects_whole_request(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"bramble.setLocationConfig\","
        "\"params\":{\"enabled\":true,\"channel_targets\":[{\"channel\":3},{\"channel\":0}]}}");
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(resp, "error"));
    cJSON_Delete(resp);

    cJSON* cfg = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":63,\"method\":\"bramble."
                                    "getConfig\",\"params\":{}}");
    cJSON* loc = cJSON_GetObjectItem(get_result(cfg), "location");
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(cJSON_GetObjectItem(loc, "channel_targets")));
    cJSON_Delete(cfg);
}

/* A private channel target is the supported configuration and still works. */
void test_set_location_config_accepts_private_channel_target(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":64,\"method\":\"bramble.setLocationConfig\","
        "\"params\":{\"enabled\":true,\"channel_targets\":[{\"channel\":2,\"tier\":\"full\"}]}}");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(get_result(resp), "ok")));
    cJSON_Delete(resp);

    cJSON* cfg = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":65,\"method\":\"bramble."
                                    "getConfig\",\"params\":{}}");
    cJSON* loc = cJSON_GetObjectItem(get_result(cfg), "location");
    cJSON* targets = cJSON_GetObjectItem(loc, "channel_targets");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(targets));
    TEST_ASSERT_EQUAL(2, cJSON_GetObjectItem(cJSON_GetArrayItem(targets, 0), "channel")->valueint);
    cJSON_Delete(cfg);
}

/* ── 2d. setWifiConfig ────────────────────────────────────────────── */

void test_set_wifi_config_missing_ssid_invalid(void) {
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"bramble.setWifiConfig\",\"params\":{}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    TEST_ASSERT_EQUAL_STRING("", g_wifi_set_creds_ssid);
    cJSON_Delete(resp);
}

void test_set_wifi_config_empty_ssid_invalid(void) {
    cJSON* resp = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":51,\"method\":\"bramble."
                                     "setWifiConfig\",\"params\":{\"ssid\":\"\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    cJSON_Delete(resp);
}

void test_set_wifi_config_oversize_ssid_invalid(void) {
    /* 33 chars, one past the 32-char SSID ceiling. */
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":52,\"method\":\"bramble.setWifiConfig\","
                           "\"params\":{\"ssid\":\"123456789012345678901234567890123\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    TEST_ASSERT_EQUAL_STRING("", g_wifi_set_creds_ssid);
    cJSON_Delete(resp);
}

void test_set_wifi_config_oversize_password_invalid(void) {
    char req[256];
    /* 65 chars, one past the 64-char WPA2 password ceiling. */
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":53,\"method\":\"bramble.setWifiConfig\","
             "\"params\":{\"ssid\":\"example-ssid\",\"password\":"
             "\"12345678901234567890123456789012345678901234567890123456789012345\"}}");
    cJSON* resp = dispatch_and_parse(req);
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32602, cJSON_GetObjectItem(err, "code")->valueint);
    TEST_ASSERT_EQUAL_STRING("", g_wifi_set_creds_ssid);
    cJSON_Delete(resp);
}

void test_set_wifi_config_bad_mode_not_supported(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":54,\"method\":\"bramble.setWifiConfig\","
                           "\"params\":{\"ssid\":\"example-ssid\",\"mode\":\"ap\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-1004, cJSON_GetObjectItem(err, "code")->valueint);
    TEST_ASSERT_EQUAL_STRING("", g_wifi_set_creds_ssid);
    cJSON_Delete(resp);
}

void test_set_wifi_config_persists_open_network(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":55,\"method\":\"bramble.setWifiConfig\","
                           "\"params\":{\"ssid\":\"example-ssid\",\"mode\":\"sta\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("reboot_required", cJSON_GetObjectItem(r, "applied")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "password"));
    TEST_ASSERT_EQUAL_STRING("example-ssid", g_wifi_set_creds_ssid);
    TEST_ASSERT_EQUAL_STRING("", g_wifi_set_creds_password);
    cJSON_Delete(resp);
}

void test_set_wifi_config_persists_both_keys(void) {
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":56,\"method\":\"bramble.setWifiConfig\","
                           "\"params\":{\"ssid\":\"example-ssid\",\"password\":\"hunter22\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_STRING("reboot_required", cJSON_GetObjectItem(r, "applied")->valuestring);
    /* The response never echoes the password back, write-only. */
    TEST_ASSERT_NULL(cJSON_GetObjectItem(r, "password"));
    TEST_ASSERT_EQUAL_STRING("example-ssid", g_wifi_set_creds_ssid);
    TEST_ASSERT_EQUAL_STRING("hunter22", g_wifi_set_creds_password);
    cJSON_Delete(resp);
}

void test_set_wifi_config_persist_failure_reports_internal_error(void) {
    g_wifi_set_creds_rc = -1;
    cJSON* resp =
        dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":57,\"method\":\"bramble.setWifiConfig\","
                           "\"params\":{\"ssid\":\"example-ssid\"}}");
    cJSON* err = get_error(resp);
    TEST_ASSERT_EQUAL_INT(-32603, cJSON_GetObjectItem(err, "code")->valueint);
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
     * s_ota_in_progress stays true, so a second call should be rejected.
     * This is also the only test in this binary that can observe a
     * successful otaUpdate call (s_ota_in_progress never resets afterward
     * since the stub never runs ota_task), so the stale-progress-reset
     * assertion below piggybacks on it rather than adding a second
     * standalone successful-call test. */

    /* Simulate a previous attempt that ended in OTA_PROG_FAILED. A poller
     * reading bramble.otaStatus right after the call below is accepted
     * (before the task's first "downloading" report) must not see that
     * stale terminal state. */
    ota_progress_report(OTA_PROG_FAILED, 10, 100);

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

    ota_progress_snapshot_t snap;
    ota_progress_get(&snap);
    TEST_ASSERT_EQUAL(OTA_PROG_IDLE, snap.state);

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

/* ── phy.tx (the actual RF-TX entrypoint) ─────────────────────────────
 *
 * These drive the real handle_phy_tx via the dispatcher and assert the two
 * security-load-bearing behaviours that the whitebox gate test could not reach
 * at the handler level: (a) phy.tx is REFUSED and never touches the tx gate when
 * passthrough is inactive; (b) when active, phy.tx routes the exact frame
 * through tx_gate_send (not radio_transmit_raw). The stubbed tx_gate_send
 * captures the call count and last frame.                                   */

void test_phy_tx_refused_when_inactive(void) {
    /* setUp left passthrough disabled -> inactive. */
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"phy.tx\",\"params\":{\"frame\":\"a1b2c3\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    /* The refusal must happen BEFORE the radio: tx gate is never reached. */
    TEST_ASSERT_EQUAL_INT(0, g_stub_tx_gate_calls);
    cJSON_Delete(resp);
}

void test_phy_tx_routes_through_tx_gate_when_active(void) {
    /* Enable passthrough (force so it activates regardless of stubbed identity),
     * then transmit. The host clock is frozen at 0, so the TTL window is open. */
    cJSON* en = dispatch_and_parse("{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"phy.enable\","
                                   "\"params\":{\"ttl_s\":600,\"force\":true}}");
    cJSON_Delete(en);

    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"phy.tx\",\"params\":{\"frame\":\"a1b2c3\"}}");
    cJSON* r = get_result(resp);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    TEST_ASSERT_EQUAL_INT(3, (int)cJSON_GetObjectItem(r, "len")->valuedouble);

    /* phy.tx reached the tx gate exactly once, with the decoded frame bytes. */
    TEST_ASSERT_EQUAL_INT(1, g_stub_tx_gate_calls);
    TEST_ASSERT_EQUAL_UINT8(3, g_stub_tx_gate_last_len);
    const uint8_t expect[] = {0xa1, 0xb2, 0xc3};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, g_stub_tx_gate_last_frame, 3);
    cJSON_Delete(resp);
}

/* ── main ─────────────────────────────────────────────────────────── */

void test_enter_dfu_unsupported_platform_reports_ok_false(void) {
    s_enter_dfu_rc = -1;
    s_enter_dfu_calls = 0;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"bramble.enterDfu\",\"params\":{}}");
    cJSON* result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL(result);
    /* OkResponse requires ok; the error path must carry ok:false + error. */
    cJSON* ok = cJSON_GetObjectItem(result, "ok");
    TEST_ASSERT_TRUE(cJSON_IsBool(ok));
    TEST_ASSERT_FALSE(cJSON_IsTrue(ok));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(result, "error")));
    TEST_ASSERT_EQUAL_INT(1, s_enter_dfu_calls);
    cJSON_Delete(resp);
}

void test_enter_dfu_supported_platform_reports_ok_true(void) {
    s_enter_dfu_rc = 0;
    s_enter_dfu_calls = 0;
    cJSON* resp = dispatch_and_parse(
        "{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"bramble.enterDfu\",\"params\":{}}");
    cJSON* result = cJSON_GetObjectItem(resp, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(result, "error"));
    TEST_ASSERT_EQUAL_INT(1, s_enter_dfu_calls);
    cJSON_Delete(resp);
}

int main(void) {
    UNITY_BEGIN();

    /* getStatus */
    RUN_TEST(test_get_status_returns_expected_fields);
    RUN_TEST(test_get_battery_returns_charging_and_present_fields);

    /* getStatus GNSS observability fields */
    RUN_TEST(test_get_status_includes_gnss_fields);
    RUN_TEST(test_get_status_gnss_fields_present_without_gps_cap);

    /* getGpsPosition */
    RUN_TEST(test_get_gps_position_includes_gnss_fields_without_fix);
    RUN_TEST(test_get_gps_position_not_supported_without_gps_cap);

    /* getDiagnostics GPS fields */
    RUN_TEST(test_get_diagnostics_includes_gps_fields_when_gps_capable);
    RUN_TEST(test_get_diagnostics_omits_gps_fields_without_gps_cap);

    /* getNeighbors */
    RUN_TEST(test_get_neighbors_empty_table);

    RUN_TEST(test_set_gps_enabled_not_supported_without_gps_cap);
    RUN_TEST(test_set_gps_enabled_missing_param_invalid);
    RUN_TEST(test_set_gps_enabled_non_bool_param_invalid);
    RUN_TEST(test_set_gps_enabled_persists_and_reads_back_false);
    RUN_TEST(test_set_gps_enabled_persists_and_reads_back_true);

    /* exportTopology */
    RUN_TEST(test_export_topology_carries_identity_radio_neighbors_and_routes);
    RUN_TEST(test_export_topology_matches_get_neighbors_and_get_routes);
    RUN_TEST(test_export_topology_reports_configured_node_name);

    /* shareLocationOnce */
    RUN_TEST(test_share_location_once_no_source_errors);
    RUN_TEST(test_share_location_once_manual_nvs_succeeds);
    RUN_TEST(test_set_location_config_rejects_public_channel_target);
    RUN_TEST(test_set_location_config_public_channel_rejects_whole_request);
    RUN_TEST(test_set_location_config_accepts_private_channel_target);

    RUN_TEST(test_set_wifi_config_missing_ssid_invalid);
    RUN_TEST(test_set_wifi_config_empty_ssid_invalid);
    RUN_TEST(test_set_wifi_config_oversize_ssid_invalid);
    RUN_TEST(test_set_wifi_config_oversize_password_invalid);
    RUN_TEST(test_set_wifi_config_bad_mode_not_supported);
    RUN_TEST(test_set_wifi_config_persists_open_network);
    RUN_TEST(test_set_wifi_config_persists_both_keys);
    RUN_TEST(test_set_wifi_config_persist_failure_reports_internal_error);

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

    /* phy.tx (RF-TX entrypoint gating + tx-gate routing) */
    RUN_TEST(test_phy_tx_refused_when_inactive);
    RUN_TEST(test_phy_tx_routes_through_tx_gate_when_active);

    /* enterDfu */
    RUN_TEST(test_enter_dfu_unsupported_platform_reports_ok_false);
    RUN_TEST(test_enter_dfu_supported_platform_reports_ok_true);

    return UNITY_END();
}
