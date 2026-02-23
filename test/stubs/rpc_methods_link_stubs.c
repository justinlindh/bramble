#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_stubs.h"
#include "nvs.h"

typedef struct { const char *short_name; } bramble_board_config_t;
typedef struct { int dummy; } mesh_shared_state_t;
typedef struct { int dummy; } routing_table_t;
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

char g_last_channel_name[64];
uint8_t g_last_channel_psk[128];
size_t g_last_channel_psk_len = 0;
int g_mesh_add_channel_calls = 0;
int g_mesh_add_channel_return = 2;

/* Configurable mesh runtime channel snapshot for getConfig tests. */
int g_mesh_channel_count = 1;
int g_mesh_default_channel = 0;
char g_mesh_channel_names[8][20] = { "Broadcast" };
bool g_mesh_channel_has_psk[8] = { false };
uint16_t g_mesh_channel_epoch[8] = { 0 };

/* Configurable NVS persistence fallback used by getConfig export tests. */
bool g_nvs_allow_open = false;
char g_nvs_node_name[64] = "";
char g_nvs_channel_names[8][20] = {{0}};
uint8_t g_nvs_channel_psk_flags[8] = {0};
bool g_nvs_channel_psk_present[8] = {false};

int mesh_add_channel(const char *name, const uint8_t *psk, size_t psk_len) {
    g_mesh_add_channel_calls++;
    if (name) {
        strncpy(g_last_channel_name, name, sizeof(g_last_channel_name) - 1);
        g_last_channel_name[sizeof(g_last_channel_name) - 1] = '\0';
    } else {
        g_last_channel_name[0] = '\0';
    }
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
void mesh_get_state(mesh_shared_state_t *o){memset(o,0,sizeof(*o));}
void mesh_get_routes(routing_table_t *o){memset(o,0,sizeof(*o));}
void mesh_set_mailbox(bool e){(void)e;} void mesh_set_node_name(const char *n){(void)n;} void mesh_reboot_delayed(uint32_t d){(void)d;} bool mesh_get_beacon_status(void){return true;} int mesh_set_beacon_policy(beacon_policy_mode_t m){(void)m;return 0;} int mesh_get_beacon_policy(beacon_policy_mode_t *m){if(m)*m=BEACON_POLICY_BALANCED;return 0;} int mesh_send_message(uint32_t d,const char *m){(void)d;(void)m;return 0;} int mesh_send_broadcast(const uint8_t *m,size_t l){(void)m;(void)l;return 0;} int mesh_send_channel(uint8_t c,const char *m){(void)c;(void)m;return 0;} int mesh_send_probe(uint32_t t,uint16_t c,bool p){(void)t;(void)c;(void)p;return 0;}
uint32_t mesh_get_last_broadcast_id(void){return 0xABCDEF01;} void mesh_set_broadcast_telemetry_mode(broadcast_telemetry_mode_t mode){(void)mode;} broadcast_telemetry_mode_t mesh_get_broadcast_telemetry_mode(void){return BROADCAST_TELEMETRY_RECIPIENT_ONLY;}
uint32_t airtime_budget_remaining(void){return 0;} void airtime_budget_refill(uint32_t n){(void)n;} uint32_t airtime_budget_next_refill_ms(void){return 0;}
int battery_read_mv(void){return 0;} int battery_read_pct(void){return 0;}
const bramble_board_config_t *board_get_config(void){return 0;}
int display_set_backlight(uint8_t level){(void)level;return 0;}
bool freq_plan_valid_freq(uint32_t f){(void)f;return true;} int8_t freq_plan_clamp_power(uint32_t f,int8_t p){(void)f;return p;} void freq_plan_get_default(uint32_t *f,int8_t *p){if(f)*f=915000;if(p)*p=14;}
void radio_get_config(radio_config_t *cfg){memset(cfg,0,sizeof(*cfg));} int radio_reconfigure(const radio_config_t *cfg){(void)cfg;return 0;}
int msg_store_count(void){return 0;} bool msg_store_get(int i, bramble_message_t *o){(void)i;(void)o;return false;}
int traffic_debug_get_count(void){return 0;} int traffic_debug_get_dropped(void){return 0;} bool traffic_debug_get_event(int i, traffic_event_t *o){(void)i;(void)o;return false;} int mesh_traffic_debug_set_config(const traffic_debug_config_t *cfg){(void)cfg;return 0;} void mesh_traffic_debug_get_config(traffic_debug_config_t *cfg){memset(cfg,0,sizeof(*cfg));} bool mesh_get_traffic_debug(void){return false;}
const esp_partition_t *ota_get_running_partition(void){return 0;} int ota_wifi_start(const char *url){(void)url;return -1;}

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out_handle){
    (void)mode;
    if (!g_nvs_allow_open || !out_handle) return ESP_FAIL;
    if (strcmp(ns, "bramble") == 0) {
        *out_handle = 1;
        return ESP_OK;
    }
    if (strcmp(ns, "bramble_ch") == 0) {
        *out_handle = 2;
        return ESP_OK;
    }
    return ESP_FAIL;
}
void nvs_close(nvs_handle_t h){(void)h;}

esp_err_t nvs_get_str(nvs_handle_t h,const char* k,char* v,size_t* l){
    if (!v || !l) return ESP_FAIL;
    if (h == 1 && strcmp(k, "node_name") == 0) {
        size_t need = strlen(g_nvs_node_name) + 1;
        if (*l < need) return ESP_FAIL;
        memcpy(v, g_nvs_node_name, need);
        *l = need;
        return ESP_OK;
    }
    if (h == 2 && strncmp(k, "nm", 2) == 0) {
        int idx = atoi(k + 2);
        if (idx >= 0 && idx < 8 && g_nvs_channel_names[idx][0]) {
            size_t need = strlen(g_nvs_channel_names[idx]) + 1;
            if (*l < need) return ESP_FAIL;
            memcpy(v, g_nvs_channel_names[idx], need);
            *l = need;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t h,const char* k,const char* v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char* k,uint8_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_u16(nvs_handle_t h,const char* k,uint16_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_u32(nvs_handle_t h,const char* k,uint32_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_i8(nvs_handle_t h,const char* k,int8_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_i32(nvs_handle_t h,const char* k,int32_t v){(void)h;(void)k;(void)v;return ESP_OK;}

esp_err_t nvs_get_u8(nvs_handle_t h,const char* k,uint8_t *o){
    if (h == 2 && o && strncmp(k, "psk", 3) == 0) {
        int idx = atoi(k + 3);
        if (idx >= 0 && idx < 8 && g_nvs_channel_psk_present[idx]) {
            *o = g_nvs_channel_psk_flags[idx];
            return ESP_OK;
        }
    }
    if (o) *o = 0;
    return ESP_FAIL;
}

esp_err_t nvs_get_u16(nvs_handle_t h,const char* k,uint16_t *o){
    (void)h; (void)k;
    if (o) *o = 0;
    return ESP_FAIL;
}

esp_err_t nvs_get_i32(nvs_handle_t h,const char* k,int32_t *o){(void)h;(void)k;if(o)*o=0;return ESP_FAIL;} esp_err_t nvs_erase_key(nvs_handle_t h,const char* k){(void)h;(void)k;return ESP_OK;} esp_err_t nvs_commit(nvs_handle_t h){(void)h;return ESP_OK;}
