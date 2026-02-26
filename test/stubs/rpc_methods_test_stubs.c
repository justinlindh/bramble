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

typedef struct { const char *short_name; } bramble_board_config_t;

typedef struct {
    uint32_t addr;
    int8_t rssi;
    int8_t snr;
    uint8_t delivery_rate;
    uint32_t airtime_remaining;
    uint32_t last_heard;
    char name[33];
} neighbor_entry_t;

typedef struct {
    int count;
    neighbor_entry_t entries[16];
} neighbor_table_t;

typedef struct {
    bool radio_ok;
    neighbor_table_t neighbors;
    uint32_t beacon_tx_count;
    uint32_t beacon_rx_count;
    uint32_t packets_tx;
    uint32_t packets_rx;
} mesh_shared_state_t;

typedef struct {
    uint32_t dest_addr;
    uint32_t next_hop;
    uint8_t hop_count;
    uint16_t metric;
    uint8_t state;
    uint32_t use_count;
} route_entry_t;

#define ROUTE_BROKEN 4

typedef struct {
    int count;
    route_entry_t entries[16];
} routing_table_t;

typedef enum { BEACON_POLICY_ALWAYS=0, BEACON_POLICY_BALANCED=1, BEACON_POLICY_MINIMAL=2 } beacon_policy_mode_t;
typedef enum { BROADCAST_TELEMETRY_OFF=0, BROADCAST_TELEMETRY_RECIPIENT_ONLY=1, BROADCAST_TELEMETRY_PATH_SAMPLED=2 } broadcast_telemetry_mode_t;

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

typedef struct { uint32_t ts_ms; uint32_t src; uint32_t dst; uint8_t type; uint8_t channel; uint8_t ttl; uint8_t flags; uint16_t len; int8_t rssi; int8_t snr; bool tx; bool accepted; } traffic_event_t;
typedef struct { bool enabled; bool include_payload; uint16_t max_events; } traffic_debug_config_t;
typedef struct { int dummy; } bramble_message_t;
typedef struct esp_partition_t { int dummy; } esp_partition_t;

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
int      g_stub_send_broadcast_return = 0;  /* 0 = success */
uint32_t g_stub_last_broadcast_id = 0xABCDEF01;

/* Channel stubs */
char g_last_channel_name[64];
uint8_t g_last_channel_psk[128];
size_t g_last_channel_psk_len = 0;
int g_mesh_add_channel_calls = 0;
int g_mesh_add_channel_return = 2;

int g_mesh_channel_count = 1;
int g_mesh_default_channel = 0;
char g_mesh_channel_names[8][20] = { "Broadcast" };
bool g_mesh_channel_has_psk[8] = { false };
uint16_t g_mesh_channel_epoch[8] = { 0 };

/* NVS */
bool g_nvs_allow_open = true;  /* default to true for our tests */
char g_nvs_node_name[64] = "";
char g_nvs_channel_names[8][20] = {{0}};
uint8_t g_nvs_channel_psk_flags[8] = {0};
bool g_nvs_channel_psk_present[8] = {false};

typedef struct { char key[16]; char value[64]; bool used; } nvs_loc_kv_t;
typedef struct { char key[16]; uint8_t value[64]; size_t len; bool used; } nvs_loc_blob_t;
nvs_loc_kv_t g_nvs_loc_kv[16];
int g_nvs_loc_kv_count = 0;
nvs_loc_blob_t g_nvs_loc_blob[16];
int g_nvs_loc_blob_count = 0;

/* ── Mesh stubs (correct signatures) ─────────────────────────────── */

int mesh_add_channel(const char *name, const uint8_t *psk, size_t psk_len) {
    g_mesh_add_channel_calls++;
    if (name) { strncpy(g_last_channel_name, name, sizeof(g_last_channel_name)-1); }
    g_last_channel_psk_len = psk_len;
    if (psk && psk_len <= sizeof(g_last_channel_psk)) memcpy(g_last_channel_psk, psk, psk_len);
    return g_mesh_add_channel_return;
}
int mesh_remove_channel(int index) { (void)index; return 0; }
int mesh_set_default_channel(int index) { (void)index; return 0; }
int mesh_get_channel_count(void) { return g_mesh_channel_count; }
int mesh_get_channel_info(int *d) { if (d) *d = g_mesh_default_channel; return g_mesh_channel_count; }
const char *mesh_get_channel_name(int i) {
    if (i < 0 || i >= g_mesh_channel_count) return NULL;
    return g_mesh_channel_names[i][0] ? g_mesh_channel_names[i] : NULL;
}
int mesh_get_channel_security(int i, bool *h, uint16_t *e) {
    if (i < 0 || i >= g_mesh_channel_count) return -1;
    if (h) *h = g_mesh_channel_has_psk[i];
    if (e) *e = g_mesh_channel_epoch[i];
    return 0;
}

void mesh_get_state(mesh_shared_state_t *o) { memset(o, 0, sizeof(*o)); }
void mesh_get_routes(routing_table_t *o) { memset(o, 0, sizeof(*o)); }
void mesh_set_mailbox(bool e) { (void)e; }
void mesh_set_node_name(const char *n) { (void)n; }
void mesh_reboot_delayed(uint32_t d) { (void)d; }
bool mesh_get_beacon_status(void) { return true; }
int mesh_set_beacon_policy(beacon_policy_mode_t m) { (void)m; return 0; }
int mesh_get_beacon_policy(beacon_policy_mode_t *m) { if (m) *m = BEACON_POLICY_BALANCED; return 0; }

/* Correct signatures matching mesh_task.h */
uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len) {
    (void)dest_addr; (void)data; (void)len;
    return g_stub_send_message_return;
}

int mesh_send_broadcast(const uint8_t *data, size_t len) {
    (void)data; (void)len;
    return g_stub_send_broadcast_return;
}

uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t *data, size_t len) {
    (void)channel_idx; (void)dest_addr; (void)data; (void)len;
    return g_stub_send_channel_return;
}

int mesh_send_probe(uint32_t t, uint16_t c, bool p) { (void)t; (void)c; (void)p; return 0; }
uint32_t mesh_get_last_broadcast_id(void) { return g_stub_last_broadcast_id; }
void mesh_set_broadcast_telemetry_mode(broadcast_telemetry_mode_t mode) { (void)mode; }
broadcast_telemetry_mode_t mesh_get_broadcast_telemetry_mode(void) { return BROADCAST_TELEMETRY_RECIPIENT_ONLY; }
bool mesh_supports_delivery_event_sync(void) { return false; }

uint32_t airtime_budget_remaining(void) { return 0; }
void airtime_budget_refill(uint32_t n) { (void)n; }
uint32_t airtime_budget_next_refill_ms(void) { return 0; }
int battery_read_mv(void) { return 3700; }
int battery_read_pct(void) { return 85; }
/* Return a zeroed-out block large enough for the real bramble_board_config_t.
 * The real struct is ~256 bytes; we allocate 512 to be safe.
 * The short_name pointer is at offset 0 in the real struct. */
static char g_stub_board_mem[512];
const void *board_get_config(void) {
    /* Ensure short_name is set (it's a const char* at some offset — we set
     * offset 0 which the stubs type expects, but real struct may differ).
     * Zero-init means capabilities=0, so board_has_cap returns false. */
    memset(g_stub_board_mem, 0, sizeof(g_stub_board_mem));
    /* We need to find where short_name lives. Since we can't include the real
     * header here, just return zeroed memory. board_has_cap will return false,
     * and bramble_hardware() will return "unknown" since short_name will be NULL. */
    return g_stub_board_mem;
}
int display_set_backlight(uint8_t level) { (void)level; return 0; }
bool freq_plan_valid_freq(uint32_t f) { (void)f; return true; }
int8_t freq_plan_clamp_power(uint32_t f, int8_t p) { (void)f; return p; }
void freq_plan_get_default(uint32_t *f, int8_t *p) { if (f) *f = 915000; if (p) *p = 14; }
void radio_get_config(radio_config_t *cfg) { memset(cfg, 0, sizeof(*cfg)); }
int radio_reconfigure(const radio_config_t *cfg) { (void)cfg; return 0; }
int msg_store_count(void) { return 0; }
bool msg_store_get(int i, bramble_message_t *o) { (void)i; (void)o; return false; }
int traffic_debug_get_count(void) { return 0; }
int traffic_debug_get_dropped(void) { return 0; }
bool traffic_debug_get_event(int i, traffic_event_t *o) { (void)i; (void)o; return false; }
int mesh_traffic_debug_set_config(const traffic_debug_config_t *cfg) { (void)cfg; return 0; }
void mesh_traffic_debug_get_config(traffic_debug_config_t *cfg) { memset(cfg, 0, sizeof(*cfg)); }
bool mesh_get_traffic_debug(void) { return false; }
const char *ota_get_running_partition(void) { return "ota_0"; }
int ota_wifi_start(const char *url) { (void)url; return -1; }
/* delivery event stubs */
typedef struct { uint32_t dummy; } delivery_event_record_t;
uint32_t mesh_delivery_events_latest_seq(void) { return 0; }
size_t mesh_delivery_events_list_since(uint32_t since, delivery_event_record_t *out, size_t max) {
    (void)since; (void)out; (void)max; return 0;
}

uint32_t mesh_send_location_packet(uint32_t dest_addr, const bramble_position_t *pos, uint8_t tier) {
    (void)dest_addr; (void)pos; (void)tier;
    return 0xABCDEF01u;
}

/* ── NVS stubs (same as rpc_methods_link_stubs) ──────────────────── */

struct nvs_iter_rec { int source; int index; };

static int nvs_loc_find_index(const char *key) {
    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_kv[i].used && strcmp(g_nvs_loc_kv[i].key, key) == 0) return i;
    }
    return -1;
}
static int nvs_loc_alloc_index(void) {
    for (int i = 0; i < 16; i++) { if (!g_nvs_loc_kv[i].used) return i; }
    return -1;
}
static int nvs_loc_blob_find_index(const char *key) {
    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_blob[i].used && strcmp(g_nvs_loc_blob[i].key, key) == 0) return i;
    }
    return -1;
}
static int nvs_loc_next_used_from(int start) {
    for (int i = start; i < 16; i++) { if (g_nvs_loc_kv[i].used) return i; }
    return -1;
}

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out) {
    (void)mode;
    if (!g_nvs_allow_open || !out) return ESP_FAIL;
    if (strcmp(ns, "bramble") == 0) { *out = 1; return ESP_OK; }
    if (strcmp(ns, "bramble_ch") == 0) { *out = 2; return ESP_OK; }
    if (strcmp(ns, "bramble_loc") == 0) { *out = 3; return ESP_OK; }
    return ESP_FAIL;
}
void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* v, size_t* l) {
    if (!l) return ESP_FAIL;
    if (h == 1 && strcmp(k, "node_name") == 0) {
        size_t need = strlen(g_nvs_node_name) + 1;
        if (!v) { *l = need; return ESP_OK; }
        if (*l < need) return ESP_FAIL;
        memcpy(v, g_nvs_node_name, need); *l = need; return ESP_OK;
    }
    if (h == 3) {
        int idx = nvs_loc_find_index(k);
        if (idx >= 0) {
            size_t need = strlen(g_nvs_loc_kv[idx].value) + 1;
            if (!v) { *l = need; return ESP_OK; }
            if (*l < need) return ESP_FAIL;
            memcpy(v, g_nvs_loc_kv[idx].value, need); *l = need; return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char* k, const char* v) {
    if (h == 1 && strcmp(k, "node_name") == 0 && v) {
        strncpy(g_nvs_node_name, v, sizeof(g_nvs_node_name)-1);
        return ESP_OK;
    }
    if (h == 3 && k && v) {
        int idx = nvs_loc_find_index(k);
        if (idx < 0) idx = nvs_loc_alloc_index();
        if (idx < 0) return ESP_FAIL;
        g_nvs_loc_kv[idx].used = true;
        strncpy(g_nvs_loc_kv[idx].key, k, sizeof(g_nvs_loc_kv[idx].key)-1);
        strncpy(g_nvs_loc_kv[idx].value, v, sizeof(g_nvs_loc_kv[idx].value)-1);
    }
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t nvs_set_u16(nvs_handle_t h, const char* k, uint16_t v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char* k, uint32_t v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t nvs_set_i8(nvs_handle_t h, const char* k, int8_t v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char* k, int32_t v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t *o) { (void)h;(void)k; if (o) *o = 0; return ESP_FAIL; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char* k, uint16_t *o) { (void)h;(void)k; if (o) *o = 0; return ESP_FAIL; }
esp_err_t nvs_get_i32(nvs_handle_t h, const char* k, int32_t *o) { (void)h;(void)k; if (o) *o = 0; return ESP_FAIL; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char* k, void *o, size_t *l) {
    if (h != 3 || !k || !l) return ESP_FAIL;
    int idx = nvs_loc_blob_find_index(k);
    if (idx < 0) return ESP_FAIL;
    if (!o) { *l = g_nvs_loc_blob[idx].len; return ESP_OK; }
    if (*l < g_nvs_loc_blob[idx].len) return ESP_FAIL;
    memcpy(o, g_nvs_loc_blob[idx].value, g_nvs_loc_blob[idx].len);
    *l = g_nvs_loc_blob[idx].len;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* k) {
    if (h == 3 && k) {
        int idx = nvs_loc_find_index(k);
        if (idx >= 0) { g_nvs_loc_kv[idx].used = false; }
        int bidx = nvs_loc_blob_find_index(k);
        if (bidx >= 0) { g_nvs_loc_blob[bidx].used = false; }
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *out) {
    (void)part; (void)ns; (void)type;
    if (!out) return ESP_FAIL;
    *out = NULL;
    nvs_iterator_t it = (nvs_iterator_t)malloc(sizeof(*it));
    if (!it) return ESP_FAIL;
    int idx = nvs_loc_next_used_from(0);
    if (idx >= 0) { it->source = 0; it->index = idx; *out = it; return ESP_OK; }
    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_blob[i].used) { it->source = 1; it->index = i; *out = it; return ESP_OK; }
    }
    free(it); return ESP_FAIL;
}
esp_err_t nvs_entry_next(nvs_iterator_t *it) {
    if (!it || !*it) return ESP_FAIL;
    if ((*it)->source == 0) {
        int idx = nvs_loc_next_used_from((*it)->index + 1);
        if (idx >= 0) { (*it)->index = idx; return ESP_OK; }
        for (int i = 0; i < 16; i++) {
            if (g_nvs_loc_blob[i].used) { (*it)->source = 1; (*it)->index = i; return ESP_OK; }
        }
    } else {
        for (int i = (*it)->index + 1; i < 16; i++) {
            if (g_nvs_loc_blob[i].used) { (*it)->index = i; return ESP_OK; }
        }
    }
    free(*it); *it = NULL; return ESP_FAIL;
}
void nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info) {
    if (!it || !info) return;
    memset(info, 0, sizeof(*info));
    if (it->source == 0) strncpy(info->key, g_nvs_loc_kv[it->index].key, sizeof(info->key)-1);
    else strncpy(info->key, g_nvs_loc_blob[it->index].key, sizeof(info->key)-1);
}
void nvs_release_iterator(nvs_iterator_t it) { if (it) free(it); }
