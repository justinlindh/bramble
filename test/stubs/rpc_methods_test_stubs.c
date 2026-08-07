/*
 * Test stubs for test_rpc_methods_firmware.c
 * Overrides from rpc_methods_link_stubs.c with correct function signatures
 * for mesh_send_message / mesh_send_channel that rpc_methods.c actually calls.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_stubs.h"
#include "nvs.h"

/* ── Types mirrored from firmware headers ─────────────────────────── */

typedef struct {
    const char* short_name;
} bramble_board_config_t;

/* neighbor_table_t / routing_table_t come from the real routing.h, and
 * mesh_shared_state_t is laid out exactly as mesh_task.h declares it, rather
 * than being mirrored with a smaller shape. The mirrored versions used to be
 * smaller than the real structs, so mesh_get_state()'s memset(o, 0, sizeof(*o))
 * cleared only the front of the caller's snapshot and left the tail
 * indeterminate. Sharing the real layout also lets the stub hand back a
 * populated neighbor table, which the scratch-isolation suite needs. */
#include "routing.h"
#include "airtime_budget.h"

typedef struct {
    neighbor_table_t neighbors;
    uint32_t beacon_tx_count;
    uint32_t beacon_rx_count;
    uint32_t packets_tx;
    uint32_t packets_rx;
    bool radio_ok;
    int16_t last_rx_rssi;
    int8_t last_rx_snr;
    airtime_budget_t airtime;
} mesh_shared_state_t;

typedef enum {
    BEACON_POLICY_ALWAYS = 0,
    BEACON_POLICY_BALANCED = 1,
    BEACON_POLICY_MINIMAL = 2
} beacon_policy_mode_t;
typedef enum {
    BROADCAST_TELEMETRY_OFF = 0,
    BROADCAST_TELEMETRY_RECIPIENT_ONLY = 1,
    BROADCAST_TELEMETRY_PATH_SAMPLED = 2
} broadcast_telemetry_mode_t;

typedef struct {
    float frequency_mhz;
    uint8_t sf;
    uint32_t bw_hz;
    uint8_t coding_rate;
    int8_t tx_power;
    uint16_t preamble;
    uint8_t sync_word;
    bool crc;
    bool explicit_header;
} radio_config_t;

typedef struct {
    uint32_t ts_ms;
    uint32_t src;
    uint32_t dst;
    uint8_t type;
    uint8_t channel;
    uint8_t ttl;
    uint8_t flags;
    uint16_t len;
    int8_t rssi;
    int8_t snr;
    bool tx;
    bool accepted;
} traffic_event_t;
typedef struct {
    bool enabled;
    bool include_payload;
    uint16_t max_events;
} traffic_debug_config_t;
typedef struct {
    int dummy;
} bramble_message_t;
typedef struct esp_partition_t {
    int dummy;
} esp_partition_t;

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2;
    uint32_t timestamp;
    bool valid;
} bramble_position_t;

/* ── Controllable globals for tests ───────────────────────────────── */

/* mesh_send_* return values (0 = failure, nonzero = packet id) */
uint32_t g_stub_send_message_return = 0x12345678;
uint32_t g_stub_send_channel_return = 0x12345678;
int g_stub_send_broadcast_return = 0; /* 0 = success */
uint32_t g_stub_last_broadcast_id = 0xABCDEF01;

/* Channel stubs */
char g_last_channel_name[64];
uint8_t g_last_channel_psk[128];
size_t g_last_channel_psk_len = 0;
int g_mesh_add_channel_calls = 0;
int g_mesh_add_channel_return = 2;

int g_mesh_channel_count = 1;
int g_mesh_default_channel = 0;
char g_mesh_channel_names[8][20] = {"Broadcast"};
bool g_mesh_channel_has_psk[8] = {false};
uint16_t g_mesh_channel_epoch[8] = {0};

/* NVS */
bool g_nvs_allow_open = true; /* default to true for our tests */
char g_nvs_node_name[64] = "";
char g_nvs_channel_names[8][20] = {{0}};
uint8_t g_nvs_channel_psk_flags[8] = {0};
bool g_nvs_channel_psk_present[8] = {false};
/* Persisted GPS power preference, keyed the same as gps_pref.c's "gps_en".
 * Default 1 (ON) mirrors gps_pref_get()'s own default-ON fallback. */
uint8_t g_nvs_gps_en = 1;
/* Manual location coordinates, keyed the same as mesh_location.c's real
 * "lat_e6"/"lon_e6" (NVS_NS_LOCATION namespace, handle 3 below). These are
 * the only two i32 keys used anywhere in main/, so a small dedicated pair of
 * globals stands in for a generic i32 store. */
bool g_nvs_lat_e6_set = false;
int32_t g_nvs_lat_e6 = 0;
bool g_nvs_lon_e6_set = false;
int32_t g_nvs_lon_e6 = 0;

typedef struct {
    char key[16];
    char value[64];
    bool used;
} nvs_loc_kv_t;
typedef struct {
    char key[16];
    uint8_t value[64];
    size_t len;
    bool used;
} nvs_loc_blob_t;
nvs_loc_kv_t g_nvs_loc_kv[16];
int g_nvs_loc_kv_count = 0;
nvs_loc_blob_t g_nvs_loc_blob[16];
int g_nvs_loc_blob_count = 0;

/* WiFi credential persistence globals (g_wifi_set_creds_rc/ssid/password) and
 * the wifi_manager_nvs_set_creds override itself now live in the standalone
 * stubs/wifi_manager_nvs_stub.c, shared with test_ws_server. */

/* ── Mesh stubs (correct signatures) ─────────────────────────────── */

int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len) {
    g_mesh_add_channel_calls++;
    if (name) {
        strncpy(g_last_channel_name, name, sizeof(g_last_channel_name) - 1);
    }
    g_last_channel_psk_len = psk_len;
    if (psk && psk_len <= sizeof(g_last_channel_psk))
        memcpy(g_last_channel_psk, psk, psk_len);
    return g_mesh_add_channel_return;
}
int mesh_remove_channel(int index) {
    (void)index;
    return 0;
}
int mesh_set_default_channel(int index) {
    (void)index;
    return 0;
}
int mesh_get_channel_count(void) { return g_mesh_channel_count; }
int mesh_get_channel_info(int* d) {
    if (d)
        *d = g_mesh_default_channel;
    return g_mesh_channel_count;
}
const char* mesh_get_channel_name(int i) {
    if (i < 0 || i >= g_mesh_channel_count)
        return NULL;
    return g_mesh_channel_names[i][0] ? g_mesh_channel_names[i] : NULL;
}
int mesh_get_channel_security(int i, bool* h, uint16_t* e) {
    if (i < 0 || i >= g_mesh_channel_count)
        return -1;
    if (h)
        *h = g_mesh_channel_has_psk[i];
    if (e)
        *e = g_mesh_channel_epoch[i];
    return 0;
}

/* Scratch-isolation support (see test_rpc_scratch_isolation.c).
 *
 * g_stub_mesh_state_fill lets a test decide what mesh_get_state() writes into
 * the caller's snapshot, and g_stub_mesh_state_after_fill is invoked once the
 * snapshot is populated but before the handler reads it. A concurrency test
 * parks one thread in that hook while a second thread runs a full
 * mesh_get_state(), which is exactly the interleaving that a shared scratch
 * buffer loses and a per-call buffer survives. Both default to NULL, so every
 * other suite keeps the plain zeroed snapshot. */
void (*g_stub_mesh_state_fill)(mesh_shared_state_t* out) = NULL;
void (*g_stub_mesh_state_after_fill)(void) = NULL;

void mesh_get_state(mesh_shared_state_t* o) {
    memset(o, 0, sizeof(*o));
    if (g_stub_mesh_state_fill) {
        g_stub_mesh_state_fill(o);
    }
    if (g_stub_mesh_state_after_fill) {
        g_stub_mesh_state_after_fill();
    }
}
void mesh_get_routes(routing_table_t* o) { memset(o, 0, sizeof(*o)); }
bool mesh_get_peer_verification(uint32_t addr, char sas_out[8], bool* verified, bool* key_changed) {
    (void)addr;
    if (sas_out)
        sas_out[0] = '\0';
    if (verified)
        *verified = false;
    if (key_changed)
        *key_changed = false;
    return false;
}
bool mesh_set_peer_verified(uint32_t addr, bool verified) {
    (void)addr;
    (void)verified;
    return false;
}
void mesh_get_identity_pin_stats(uint32_t* pins, uint32_t* conflicts, uint32_t* sig_failures,
                                 uint32_t* addr_mismatches, uint32_t* unendorsed,
                                 uint32_t* expired) {
    if (pins)
        *pins = 0;
    if (conflicts)
        *conflicts = 0;
    if (sig_failures)
        *sig_failures = 0;
    if (addr_mismatches)
        *addr_mismatches = 0;
    if (unendorsed)
        *unendorsed = 0;
    if (expired)
        *expired = 0;
}
void mesh_set_pin_anchor(const uint8_t* anchor_pub) { (void)anchor_pub; }
bool g_stub_mailbox_enabled = false;
void mesh_set_mailbox(bool e) { g_stub_mailbox_enabled = e; }
bool mesh_get_mailbox(void) { return g_stub_mailbox_enabled; }
bool g_stub_flood_transport_enabled = false;
void mesh_set_flood_transport(bool e) { g_stub_flood_transport_enabled = e; }
bool mesh_get_flood_transport(void) { return g_stub_flood_transport_enabled; }
uint8_t g_stub_flood_hop_limit = 8;
void mesh_set_flood_hop_limit(uint32_t h) {
    /* Mirror the real clamp to [1,32] so the RPC test sees the applied value. */
    g_stub_flood_hop_limit = (uint8_t)(h < 1 ? 1 : (h > 32 ? 32 : h));
}
uint8_t mesh_get_flood_hop_limit(void) { return g_stub_flood_hop_limit; }
void mesh_set_node_name(const char* n) { (void)n; }
void mesh_reboot_delayed(uint32_t d) { (void)d; }
void mesh_rederive_beacon_key(void) {}
void mesh_trigger_attestation(void) {}
/* Airtime backpressure counters (issues #75, #87). */
uint32_t mesh_get_flood_relay_drops(void) { return 0; }
void mesh_get_probe_ingress_stats(uint32_t* accepted, uint32_t* dropped_reply,
                                  uint32_t* dropped_forward) {
    if (accepted)
        *accepted = 0;
    if (dropped_reply)
        *dropped_reply = 0;
    if (dropped_forward)
        *dropped_forward = 0;
}
bool mesh_get_beacon_status(void) { return true; }
int mesh_set_beacon_policy(beacon_policy_mode_t m) {
    (void)m;
    return 0;
}
int mesh_get_beacon_policy(beacon_policy_mode_t* m) {
    if (m)
        *m = BEACON_POLICY_BALANCED;
    return 0;
}

/* Correct signatures matching mesh_task.h */
uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len) {
    (void)dest_addr;
    (void)data;
    (void)len;
    return g_stub_send_message_return;
}

int mesh_send_broadcast(const uint8_t* data, size_t len) {
    (void)data;
    (void)len;
    return g_stub_send_broadcast_return;
}

uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t* data, size_t len) {
    (void)channel_idx;
    (void)dest_addr;
    (void)data;
    (void)len;
    return g_stub_send_channel_return;
}

int mesh_send_probe(uint32_t t, uint16_t c, bool p) {
    (void)t;
    (void)c;
    (void)p;
    return 0;
}
uint32_t mesh_get_last_broadcast_id(void) { return g_stub_last_broadcast_id; }
void mesh_set_broadcast_telemetry_mode(broadcast_telemetry_mode_t mode) { (void)mode; }
broadcast_telemetry_mode_t mesh_get_broadcast_telemetry_mode(void) {
    return BROADCAST_TELEMETRY_RECIPIENT_ONLY;
}
bool mesh_supports_delivery_event_sync(void) { return false; }

/* Signatures follow airtime_budget.h. They previously took no arguments while
 * rpc_methods.c called them with two, which only linked because the mismatch
 * was never visible in one translation unit. */
uint32_t airtime_budget_remaining(const airtime_budget_t* ab, uint8_t tier) {
    (void)ab;
    (void)tier;
    return 0;
}
void airtime_budget_refill(airtime_budget_t* ab, uint32_t now_ms) {
    (void)ab;
    (void)now_ms;
}
uint32_t airtime_budget_next_refill_ms(const airtime_budget_t* ab, uint32_t now_ms) {
    (void)ab;
    (void)now_ms;
    return 0;
}
int battery_read_mv(void) { return 3700; }
int battery_read_pct(void) { return 85; }
/* Return a zeroed-out block large enough for the real bramble_board_config_t.
 * The real struct is ~256 bytes; we allocate 512 to be safe.
 * The short_name pointer is at offset 0 in the real struct. */
static char g_stub_board_mem[512];
/* Test lever: when true, board_get_config() reports BOARD_CAP_GPS so
 * handle_set_gps_enabled / handle_get_gps_position exercise their
 * GPS-capable path instead of the not-supported early return. */
bool g_stub_board_has_gps = false;
const void* board_get_config(void) {
    /* Ensure short_name is set (it's a const char* at some offset: we set
     * offset 0 which the stubs type expects, but real struct may differ).
     * Zero-init means capabilities=0, so board_has_cap returns false. */
    memset(g_stub_board_mem, 0, sizeof(g_stub_board_mem));
    if (g_stub_board_has_gps) {
        /* Mirror only the real bramble_board_config_t's leading fields (name,
         * short_name, capabilities; see board_config.h) so board_has_cap()
         * sees BOARD_CAP_GPS without pulling in the full ESP-only header. */
        struct {
            const char* name;
            const char* short_name;
            uint32_t capabilities;
        }* hdr = (void*)g_stub_board_mem;
        hdr->capabilities = (1u << 4); /* BOARD_CAP_GPS */
    }
    /* We need to find where short_name lives. Since we can't include the real
     * header here, just return zeroed memory. board_has_cap will return false,
     * and bramble_hardware() will return "unknown" since short_name will be NULL. */
    return g_stub_board_mem;
}
int display_set_backlight(uint8_t level) {
    (void)level;
    return 0;
}
bool freq_plan_valid_freq(uint32_t f) {
    (void)f;
    return true;
}
int8_t freq_plan_clamp_power(uint32_t f, int8_t p) {
    (void)f;
    return p;
}
void freq_plan_get_default(uint32_t* f, int8_t* p) {
    if (f)
        *f = 915000;
    if (p)
        *p = 14;
}
void radio_get_config(radio_config_t* cfg) { memset(cfg, 0, sizeof(*cfg)); }
/* PHY passthrough (phy.tx) routes through the tx gate. Capture the call so the
 * handler test can assert phy.tx actually reaches tx_gate with the exact frame
 * (and, when inactive, that it is NOT reached at all). */
int g_stub_tx_gate_calls = 0;
uint8_t g_stub_tx_gate_last_frame[255];
uint8_t g_stub_tx_gate_last_len = 0;
int g_stub_tx_gate_return = 0; /* TX_GATE_OK */
int tx_gate_send(const uint8_t* buf, uint8_t len, int kind) {
    (void)kind;
    g_stub_tx_gate_calls++;
    g_stub_tx_gate_last_len = len;
    if (buf && len) {
        memcpy(g_stub_tx_gate_last_frame, buf, len);
    }
    return g_stub_tx_gate_return;
}
int radio_reconfigure(const radio_config_t* cfg) {
    (void)cfg;
    return 0;
}
int msg_store_count(void) { return 0; }
bool msg_store_get(int i, bramble_message_t* o) {
    (void)i;
    (void)o;
    return false;
}
int traffic_debug_get_count(void) { return 0; }
int traffic_debug_get_dropped(void) { return 0; }
bool traffic_debug_get_event(int i, traffic_event_t* o) {
    (void)i;
    (void)o;
    return false;
}
int mesh_traffic_debug_set_config(const traffic_debug_config_t* cfg) {
    (void)cfg;
    return 0;
}
void mesh_traffic_debug_get_config(traffic_debug_config_t* cfg) { memset(cfg, 0, sizeof(*cfg)); }
bool mesh_get_traffic_debug(void) { return false; }
const char* traffic_debug_category_name(int category) {
    (void)category;
    return "other";
}
const char* traffic_debug_airtime_tier_name(uint8_t tier) {
    (void)tier;
    return "none";
}
const char* ota_get_running_partition(void) { return "ota_0"; }
/* ── OTA stubs (URL policy via real ota_url.c, linked into these bins) ── */
#include "ota_origin.h"
#include "ota_url.h"

char g_ota_last_url[256];
bool g_ota_last_allow_downgrade = false;
int g_ota_wifi_start_calls = 0;
char g_ota_origin_stub[256] = "https://bramblemesh.org/ota/";
bool g_ota_origin_overridden_stub = false;

int ota_wifi_start(const char* url, bool allow_downgrade) {
    snprintf(g_ota_last_url, sizeof(g_ota_last_url), "%s", url);
    g_ota_last_allow_downgrade = allow_downgrade;
    g_ota_wifi_start_calls++;
    return -1;
}
const char* ota_get_last_error(void) { return NULL; }
typedef struct {
    const char* version;
} stub_app_desc_t;
static const stub_app_desc_t s_stub_app_desc = {.version = "1.2.3"};
const void* esp_app_get_description(void) { return &s_stub_app_desc; }
const char* ota_get_app_version(void) { return "1.2.3"; }
void ota_origin_get(char* out, size_t out_len) { snprintf(out, out_len, "%s", g_ota_origin_stub); }
int ota_origin_set(const char* origin) {
    if (!ota_url_origin_valid(origin, false)) {
        return -1;
    }
    snprintf(g_ota_origin_stub, sizeof(g_ota_origin_stub), "%s", origin);
    g_ota_origin_overridden_stub = true;
    return 0;
}
int ota_origin_reset(void) {
    snprintf(g_ota_origin_stub, sizeof(g_ota_origin_stub), "%s", OTA_DEFAULT_ORIGIN);
    g_ota_origin_overridden_stub = false;
    return 0;
}
bool ota_origin_is_overridden(void) { return g_ota_origin_overridden_stub; }
int ota_resolve_artifact(const char* rel_path, char* out, size_t out_len) {
    return ota_url_resolve(g_ota_origin_stub, rel_path, false, out, out_len);
}
bool ota_rollback_get_floor(char* out, size_t out_len) {
    (void)out;
    (void)out_len;
    return false;
}
/* delivery event stubs */
typedef struct {
    uint32_t dummy;
} delivery_event_record_t;
uint32_t mesh_delivery_events_latest_seq(void) { return 0; }
size_t mesh_delivery_events_list_since(uint32_t since, delivery_event_record_t* out, size_t max) {
    (void)since;
    (void)out;
    (void)max;
    return 0;
}

uint32_t mesh_send_location_packet(uint32_t dest_addr, const bramble_position_t* pos,
                                   uint8_t tier) {
    (void)dest_addr;
    (void)pos;
    (void)tier;
    return 0xABCDEF01u;
}

/* mesh_resolve_self_position stub: mirrors mesh_location.c's real resolver
 * (live GPS first, manual NVS lat/lon fallback) without pulling in
 * mesh_location.c's mesh-task dependencies. gps_get_position is the real
 * components/gps/gps.c function, compiled into this suite; its host build
 * always reports no fix, so only the manual-NVS leg and the no-source error
 * are reachable from here (the live-GPS leg is bench-only, see Task 11). */
extern bool gps_get_position(bramble_position_t* out);

bool mesh_resolve_self_position(bramble_position_t* out) {
    bramble_position_t gps_pos;
    if (gps_get_position(&gps_pos) && gps_pos.valid) {
        *out = gps_pos;
        return true;
    }

    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    bool has_manual = (nvs_get_i32(nvs, "lat_e6", &lat_e6) == ESP_OK) &&
                      (nvs_get_i32(nvs, "lon_e6", &lon_e6) == ESP_OK) &&
                      !(lat_e6 == 0 && lon_e6 == 0);
    nvs_close(nvs);
    if (!has_manual) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->latitude_e7 = lat_e6 * 10;
    out->longitude_e7 = lon_e6 * 10;
    out->valid = true;
    return true;
}

/* ── NVS stubs (same as rpc_methods_link_stubs) ──────────────────── */

struct nvs_iter_rec {
    int source;
    int index;
};

static int nvs_loc_find_index(const char* key) {
    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_kv[i].used && strcmp(g_nvs_loc_kv[i].key, key) == 0)
            return i;
    }
    return -1;
}
static int nvs_loc_alloc_index(void) {
    for (int i = 0; i < 16; i++) {
        if (!g_nvs_loc_kv[i].used)
            return i;
    }
    return -1;
}
static int nvs_loc_blob_find_index(const char* key) {
    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_blob[i].used && strcmp(g_nvs_loc_blob[i].key, key) == 0)
            return i;
    }
    return -1;
}
static int nvs_loc_next_used_from(int start) {
    for (int i = start; i < 16; i++) {
        if (g_nvs_loc_kv[i].used)
            return i;
    }
    return -1;
}

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out) {
    (void)mode;
    if (!g_nvs_allow_open || !out)
        return ESP_FAIL;
    if (strcmp(ns, "bramble") == 0) {
        *out = 1;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_ch") == 0) {
        *out = 2;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_loc") == 0) {
        *out = 3;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_mb") == 0) {
        *out = 4;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_flood") == 0) {
        *out = 5;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_radio") == 0) {
        *out = 6;
        return ESP_OK;
    }
    return ESP_FAIL;
}
void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* v, size_t* l) {
    if (!l)
        return ESP_FAIL;
    if (h == 1 && strcmp(k, "node_name") == 0) {
        size_t need = strlen(g_nvs_node_name) + 1;
        if (!v) {
            *l = need;
            return ESP_OK;
        }
        if (*l < need)
            return ESP_FAIL;
        memcpy(v, g_nvs_node_name, need);
        *l = need;
        return ESP_OK;
    }
    if (h == 3) {
        int idx = nvs_loc_find_index(k);
        if (idx >= 0) {
            size_t need = strlen(g_nvs_loc_kv[idx].value) + 1;
            if (!v) {
                *l = need;
                return ESP_OK;
            }
            if (*l < need)
                return ESP_FAIL;
            memcpy(v, g_nvs_loc_kv[idx].value, need);
            *l = need;
            return ESP_OK;
        }
        /* location_policy_load_or_init (rpc_methods.c) distinguishes a
         * genuinely missing key (write defaults) from a real read error
         * (propagate); real NVS reports the former as
         * ESP_ERR_NVS_NOT_FOUND, not the generic ESP_FAIL. */
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char* k, const char* v) {
    if (h == 1 && strcmp(k, "node_name") == 0 && v) {
        strncpy(g_nvs_node_name, v, sizeof(g_nvs_node_name) - 1);
        return ESP_OK;
    }
    if (h == 3 && k && v) {
        int idx = nvs_loc_find_index(k);
        if (idx < 0)
            idx = nvs_loc_alloc_index();
        if (idx < 0)
            return ESP_FAIL;
        g_nvs_loc_kv[idx].used = true;
        strncpy(g_nvs_loc_kv[idx].key, k, sizeof(g_nvs_loc_kv[idx].key) - 1);
        strncpy(g_nvs_loc_kv[idx].value, v, sizeof(g_nvs_loc_kv[idx].value) - 1);
    }
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) {
    if (h == 1 && k && strcmp(k, "gps_en") == 0) {
        g_nvs_gps_en = v;
        return ESP_OK;
    }
    (void)h;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char* k, uint16_t v) {
    (void)h;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char* k, uint32_t v) {
    (void)h;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char* k, uint32_t* o) {
    (void)h;
    (void)k;
    (void)o;
    return ESP_FAIL;
}
esp_err_t nvs_set_i8(nvs_handle_t h, const char* k, int8_t v) {
    (void)h;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char* k, int32_t v) {
    if (h == 3 && k && strcmp(k, "lat_e6") == 0) {
        g_nvs_lat_e6 = v;
        g_nvs_lat_e6_set = true;
        return ESP_OK;
    }
    if (h == 3 && k && strcmp(k, "lon_e6") == 0) {
        g_nvs_lon_e6 = v;
        g_nvs_lon_e6_set = true;
        return ESP_OK;
    }
    (void)h;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* k, const void* v, size_t l) {
    (void)h;
    (void)k;
    (void)v;
    (void)l;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* o) {
    if (h == 1 && k && strcmp(k, "gps_en") == 0) {
        if (o)
            *o = g_nvs_gps_en;
        return ESP_OK;
    }
    if (o)
        *o = 0;
    /* See nvs_get_str's h==3 comment: location_policy_load_or_init needs
     * "key not present" reported as ESP_ERR_NVS_NOT_FOUND. */
    return (h == 3) ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char* k, uint16_t* o) {
    (void)k;
    if (o)
        *o = 0;
    return (h == 3) ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char* k, int32_t* o) {
    if (h == 3 && k && strcmp(k, "lat_e6") == 0 && g_nvs_lat_e6_set) {
        if (o)
            *o = g_nvs_lat_e6;
        return ESP_OK;
    }
    if (h == 3 && k && strcmp(k, "lon_e6") == 0 && g_nvs_lon_e6_set) {
        if (o)
            *o = g_nvs_lon_e6;
        return ESP_OK;
    }
    (void)h;
    (void)k;
    if (o)
        *o = 0;
    return ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* k, void* o, size_t* l) {
    if (h != 3 || !k || !l)
        return ESP_FAIL;
    int idx = nvs_loc_blob_find_index(k);
    if (idx < 0)
        return ESP_FAIL;
    if (!o) {
        *l = g_nvs_loc_blob[idx].len;
        return ESP_OK;
    }
    if (*l < g_nvs_loc_blob[idx].len)
        return ESP_FAIL;
    memcpy(o, g_nvs_loc_blob[idx].value, g_nvs_loc_blob[idx].len);
    *l = g_nvs_loc_blob[idx].len;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* k) {
    if (h == 3 && k) {
        int idx = nvs_loc_find_index(k);
        if (idx >= 0) {
            g_nvs_loc_kv[idx].used = false;
        }
        int bidx = nvs_loc_blob_find_index(k);
        if (bidx >= 0) {
            g_nvs_loc_blob[bidx].used = false;
        }
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return ESP_OK;
}

esp_err_t nvs_entry_find(const char* part, const char* ns, nvs_type_t type, nvs_iterator_t* out) {
    (void)part;
    (void)ns;
    (void)type;
    if (!out)
        return ESP_FAIL;
    *out = NULL;
    nvs_iterator_t it = (nvs_iterator_t)malloc(sizeof(*it));
    if (!it)
        return ESP_FAIL;
    int idx = nvs_loc_next_used_from(0);
    if (idx >= 0) {
        it->source = 0;
        it->index = idx;
        *out = it;
        return ESP_OK;
    }
    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_blob[i].used) {
            it->source = 1;
            it->index = i;
            *out = it;
            return ESP_OK;
        }
    }
    free(it);
    return ESP_FAIL;
}
esp_err_t nvs_entry_next(nvs_iterator_t* it) {
    if (!it || !*it)
        return ESP_FAIL;
    if ((*it)->source == 0) {
        int idx = nvs_loc_next_used_from((*it)->index + 1);
        if (idx >= 0) {
            (*it)->index = idx;
            return ESP_OK;
        }
        for (int i = 0; i < 16; i++) {
            if (g_nvs_loc_blob[i].used) {
                (*it)->source = 1;
                (*it)->index = i;
                return ESP_OK;
            }
        }
    } else {
        for (int i = (*it)->index + 1; i < 16; i++) {
            if (g_nvs_loc_blob[i].used) {
                (*it)->index = i;
                return ESP_OK;
            }
        }
    }
    free(*it);
    *it = NULL;
    return ESP_FAIL;
}
void nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* info) {
    if (!it || !info)
        return;
    memset(info, 0, sizeof(*info));
    if (it->source == 0)
        strncpy(info->key, g_nvs_loc_kv[it->index].key, sizeof(info->key) - 1);
    else
        strncpy(info->key, g_nvs_loc_blob[it->index].key, sizeof(info->key) - 1);
}
void nvs_release_iterator(nvs_iterator_t it) {
    if (it)
        free(it);
}

void ws_server_load_token(void) {}
void ws_server_load_origins(void) {}
const char* ws_server_get_extra_origins(void) { return ""; }
