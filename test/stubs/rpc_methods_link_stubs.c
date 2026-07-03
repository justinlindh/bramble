#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_stubs.h"
#include "nvs.h"
#include "esp_wifi.h"

typedef struct {
    uint32_t event_seq;
    uint32_t broadcast_id;
    uint32_t timestamp;
    uint8_t event_type;
    uint8_t tier;
    uint8_t route_len;
    uint8_t reserved0;
    uint32_t route_hops[4];
} delivery_event_record_t;
typedef struct {
    const char* short_name;
} bramble_board_config_t;
typedef struct {
    int dummy;
} mesh_shared_state_t;
typedef struct {
    int dummy;
} routing_table_t;
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

char g_last_channel_name[64];
uint8_t g_last_channel_psk[128];
size_t g_last_channel_psk_len = 0;
int g_mesh_add_channel_calls = 0;
int g_mesh_add_channel_return = 2;

/* Configurable mesh runtime channel snapshot for getConfig tests. */
int g_mesh_channel_count = 1;
int g_mesh_default_channel = 0;
char g_mesh_channel_names[8][20] = {"Broadcast"};
bool g_mesh_channel_has_psk[8] = {false};
uint16_t g_mesh_channel_epoch[8] = {0};

/* Configurable NVS persistence fallback used by getConfig export tests. */
bool g_nvs_allow_open = false;
char g_nvs_node_name[64] = "";
char g_nvs_channel_names[8][20] = {{0}};
uint8_t g_nvs_channel_psk_flags[8] = {0};
bool g_nvs_channel_psk_present[8] = {false};

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

int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len) {
    g_mesh_add_channel_calls++;
    if (name) {
        strncpy(g_last_channel_name, name, sizeof(g_last_channel_name) - 1);
        g_last_channel_name[sizeof(g_last_channel_name) - 1] = '\0';
    } else {
        g_last_channel_name[0] = '\0';
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
void mesh_get_state(mesh_shared_state_t* o) { memset(o, 0, sizeof(*o)); }
void mesh_get_routes(routing_table_t* o) { memset(o, 0, sizeof(*o)); }
bool g_stub_mailbox_enabled = false;
void mesh_set_mailbox(bool e) { g_stub_mailbox_enabled = e; }
bool mesh_get_mailbox(void) { return g_stub_mailbox_enabled; }
void mesh_set_node_name(const char* n) { (void)n; }
void mesh_reboot_delayed(uint32_t d) { (void)d; }
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
uint32_t mesh_send_message(uint32_t d, const uint8_t* data, size_t len) {
    (void)d;
    (void)data;
    (void)len;
    return 0x12345678;
}
int mesh_send_broadcast(const uint8_t* m, size_t l) {
    (void)m;
    (void)l;
    return 0;
}
uint32_t mesh_send_channel(int ch, uint32_t dest, const uint8_t* data, size_t len) {
    (void)ch;
    (void)dest;
    (void)data;
    (void)len;
    return 0x12345678;
}
int mesh_send_probe(uint32_t t, uint16_t c, bool p) {
    (void)t;
    (void)c;
    (void)p;
    return 0;
}
uint32_t mesh_get_last_broadcast_id(void) { return 0xABCDEF01; }
void mesh_set_broadcast_telemetry_mode(broadcast_telemetry_mode_t mode) { (void)mode; }
broadcast_telemetry_mode_t mesh_get_broadcast_telemetry_mode(void) {
    return BROADCAST_TELEMETRY_RECIPIENT_ONLY;
}
uint32_t airtime_budget_remaining(void) { return 0; }
void airtime_budget_refill(uint32_t n) { (void)n; }
uint32_t airtime_budget_next_refill_ms(void) { return 0; }
int battery_read_mv(void) { return 0; }
int battery_read_pct(void) { return 0; }
const bramble_board_config_t* board_get_config(void) { return 0; }
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
const char* ota_get_running_partition(void) { return "factory"; }
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
const char* addr_hex(uint32_t addr, char* buf, size_t len) {
    snprintf(buf, len, "%08X", addr);
    return buf;
}
bool mesh_supports_delivery_event_sync(void) { return false; }
uint32_t mesh_delivery_events_latest_seq(void) { return 0; }
size_t mesh_delivery_events_list_since(uint32_t since_seq, delivery_event_record_t* out,
                                       size_t max) {
    (void)since_seq;
    (void)out;
    (void)max;
    return 0;
}

struct nvs_iter_rec {
    int source; /* 0 = kv, 1 = blob */
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

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out_handle) {
    (void)mode;
    if (!g_nvs_allow_open || !out_handle)
        return ESP_FAIL;
    if (strcmp(ns, "bramble") == 0) {
        *out_handle = 1;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_ch") == 0) {
        *out_handle = 2;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_loc") == 0) {
        *out_handle = 3;
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
    if (h == 2 && strncmp(k, "nm", 2) == 0) {
        int idx = atoi(k + 2);
        if (idx >= 0 && idx < 8 && g_nvs_channel_names[idx][0]) {
            size_t need = strlen(g_nvs_channel_names[idx]) + 1;
            if (!v) {
                *l = need;
                return ESP_OK;
            }
            if (*l < need)
                return ESP_FAIL;
            memcpy(v, g_nvs_channel_names[idx], need);
            *l = need;
            return ESP_OK;
        }
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
    }
    return ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char* k, const char* v) {
    if (h != 3 || !k || !v)
        return ESP_OK;
    int idx = nvs_loc_find_index(k);
    if (idx < 0)
        idx = nvs_loc_alloc_index();
    if (idx < 0)
        return ESP_FAIL;
    if (!g_nvs_loc_kv[idx].used) {
        g_nvs_loc_kv[idx].used = true;
        g_nvs_loc_kv_count++;
    }
    strncpy(g_nvs_loc_kv[idx].key, k, sizeof(g_nvs_loc_kv[idx].key) - 1);
    g_nvs_loc_kv[idx].key[sizeof(g_nvs_loc_kv[idx].key) - 1] = '\0';
    strncpy(g_nvs_loc_kv[idx].value, v, sizeof(g_nvs_loc_kv[idx].value) - 1);
    g_nvs_loc_kv[idx].value[sizeof(g_nvs_loc_kv[idx].value) - 1] = '\0';
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) {
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
esp_err_t nvs_set_i8(nvs_handle_t h, const char* k, int8_t v) {
    (void)h;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char* k, int32_t v) {
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
    if (h == 2 && o && strncmp(k, "psk", 3) == 0) {
        int idx = atoi(k + 3);
        if (idx >= 0 && idx < 8 && g_nvs_channel_psk_present[idx]) {
            *o = g_nvs_channel_psk_flags[idx];
            return ESP_OK;
        }
    }
    if (o)
        *o = 0;
    return ESP_FAIL;
}

esp_err_t nvs_get_u16(nvs_handle_t h, const char* k, uint16_t* o) {
    (void)h;
    (void)k;
    if (o)
        *o = 0;
    return ESP_FAIL;
}

esp_err_t nvs_get_i32(nvs_handle_t h, const char* k, int32_t* o) {
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
            g_nvs_loc_kv[idx].key[0] = '\0';
            g_nvs_loc_kv[idx].value[0] = '\0';
            if (g_nvs_loc_kv_count > 0)
                g_nvs_loc_kv_count--;
        }
        int bidx = nvs_loc_blob_find_index(k);
        if (bidx >= 0) {
            g_nvs_loc_blob[bidx].used = false;
            g_nvs_loc_blob[bidx].key[0] = '\0';
            g_nvs_loc_blob[bidx].len = 0;
            if (g_nvs_loc_blob_count > 0)
                g_nvs_loc_blob_count--;
        }
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return ESP_OK;
}

esp_err_t nvs_entry_find(const char* part_name, const char* namespace_name, nvs_type_t type,
                         nvs_iterator_t* out_iterator) {
    (void)part_name;
    (void)namespace_name;
    (void)type;
    if (!out_iterator)
        return ESP_FAIL;
    *out_iterator = NULL;

    nvs_iterator_t it = (nvs_iterator_t)malloc(sizeof(*it));
    if (!it)
        return ESP_FAIL;

    int idx = nvs_loc_next_used_from(0);
    if (idx >= 0) {
        it->source = 0;
        it->index = idx;
        *out_iterator = it;
        return ESP_OK;
    }

    for (int i = 0; i < 16; i++) {
        if (g_nvs_loc_blob[i].used) {
            it->source = 1;
            it->index = i;
            *out_iterator = it;
            return ESP_OK;
        }
    }

    free(it);
    return ESP_FAIL;
}

esp_err_t nvs_entry_next(nvs_iterator_t* iterator) {
    if (!iterator || !*iterator)
        return ESP_FAIL;

    if ((*iterator)->source == 0) {
        int idx = nvs_loc_next_used_from((*iterator)->index + 1);
        if (idx >= 0) {
            (*iterator)->index = idx;
            return ESP_OK;
        }
        for (int i = 0; i < 16; i++) {
            if (g_nvs_loc_blob[i].used) {
                (*iterator)->source = 1;
                (*iterator)->index = i;
                return ESP_OK;
            }
        }
    } else {
        for (int i = (*iterator)->index + 1; i < 16; i++) {
            if (g_nvs_loc_blob[i].used) {
                (*iterator)->index = i;
                return ESP_OK;
            }
        }
    }

    free(*iterator);
    *iterator = NULL;
    return ESP_FAIL;
}

void nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t* out_info) {
    if (!iterator || !out_info)
        return;
    memset(out_info, 0, sizeof(*out_info));
    if (iterator->source == 0) {
        strncpy(out_info->key, g_nvs_loc_kv[iterator->index].key, sizeof(out_info->key) - 1);
    } else {
        strncpy(out_info->key, g_nvs_loc_blob[iterator->index].key, sizeof(out_info->key) - 1);
    }
}

void nvs_release_iterator(nvs_iterator_t iterator) {
    if (iterator)
        free(iterator);
}

uint32_t mesh_send_location_packet(uint32_t dest_addr, const bramble_position_t* pos,
                                   uint8_t tier) {
    (void)dest_addr;
    (void)pos;
    (void)tier;
    return 0xABCDEF01u;
}

void ws_server_load_token(void) {}
void ws_server_load_origins(void) {}
const char* ws_server_get_extra_origins(void) { return ""; }
const char* ws_server_get_token(void) { return ""; }
esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]) {
    (void)ifx;
    (void)mac;
    return 1; /* ESP_FAIL */
}
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* list) {
    (void)list;
    return 1; /* ESP_FAIL */
}
