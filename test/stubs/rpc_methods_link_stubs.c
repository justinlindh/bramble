#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_stubs.h"
#include "nvs.h"

typedef struct { const char *short_name; } bramble_board_config_t;
typedef struct { int dummy; } mesh_shared_state_t;
typedef struct { int dummy; } routing_table_t;
typedef enum { BEACON_POLICY_ALWAYS=0, BEACON_POLICY_BALANCED=1, BEACON_POLICY_MINIMAL=2 } beacon_policy_mode_t;
typedef struct { double frequency_mhz; int sf; int bw_hz; int tx_power; int coding_rate; } radio_config_t;
typedef struct { uint32_t ts_ms; uint32_t src; uint32_t dst; uint8_t type; uint8_t channel; uint8_t ttl; uint8_t flags; uint16_t len; int8_t rssi; int8_t snr; bool tx; bool accepted; } traffic_event_t;
typedef struct { bool enabled; bool include_payload; uint16_t max_events; } traffic_debug_config_t;
typedef struct { int dummy; } bramble_message_t;
typedef struct esp_partition_t { int dummy; } esp_partition_t;

char g_last_channel_name[64];
uint8_t g_last_channel_psk[128];
size_t g_last_channel_psk_len = 0;
int g_mesh_add_channel_calls = 0;
int g_mesh_add_channel_return = 2;

int mesh_add_channel(const char *name, const uint8_t *psk, size_t psk_len) { g_mesh_add_channel_calls++; if (name){strncpy(g_last_channel_name,name,sizeof(g_last_channel_name)-1); g_last_channel_name[sizeof(g_last_channel_name)-1]='\0';} else g_last_channel_name[0]='\0'; g_last_channel_psk_len=psk_len; if(psk&&psk_len<=sizeof(g_last_channel_psk)) memcpy(g_last_channel_psk,psk,psk_len); return g_mesh_add_channel_return; }
int mesh_remove_channel(int index){(void)index;return 0;} int mesh_set_default_channel(int index){(void)index;return 0;} int mesh_get_channel_count(void){return 1;} int mesh_get_channel_info(int *d){if(d)*d=0;return 1;} const char *mesh_get_channel_name(int i){(void)i;return "Broadcast";} int mesh_get_channel_security(int i,bool *h,uint16_t *e){(void)i;if(h)*h=false;if(e)*e=0;return 0;} void mesh_get_state(mesh_shared_state_t *o){memset(o,0,sizeof(*o));} void mesh_get_routes(routing_table_t *o){memset(o,0,sizeof(*o));}
void mesh_set_mailbox(bool e){(void)e;} void mesh_set_node_name(const char *n){(void)n;} void mesh_reboot_delayed(uint32_t d){(void)d;} bool mesh_get_beacon_status(void){return true;} int mesh_set_beacon_policy(beacon_policy_mode_t m){(void)m;return 0;} int mesh_get_beacon_policy(beacon_policy_mode_t *m){if(m)*m=BEACON_POLICY_BALANCED;return 0;} int mesh_send_message(uint32_t d,const char *m){(void)d;(void)m;return 0;} int mesh_send_broadcast(const char *m){(void)m;return 0;} int mesh_send_channel(uint8_t c,const char *m){(void)c;(void)m;return 0;} int mesh_send_probe(uint32_t t,uint16_t c,bool p){(void)t;(void)c;(void)p;return 0;}
uint32_t airtime_budget_remaining(void){return 0;} void airtime_budget_refill(uint32_t n){(void)n;} uint32_t airtime_budget_next_refill_ms(void){return 0;}
int battery_read_mv(void){return 0;} int battery_read_pct(void){return 0;}
const bramble_board_config_t *board_get_config(void){return 0;}
int display_set_backlight(uint8_t level){(void)level;return 0;}
bool freq_plan_valid_freq(uint32_t f){(void)f;return true;} int8_t freq_plan_clamp_power(uint32_t f,int8_t p){(void)f;return p;} void freq_plan_get_default(uint32_t *f,int8_t *p){if(f)*f=915000;if(p)*p=14;}
void radio_get_config(radio_config_t *cfg){memset(cfg,0,sizeof(*cfg));} int radio_reconfigure(const radio_config_t *cfg){(void)cfg;return 0;}
int msg_store_count(void){return 0;} bool msg_store_get(int i, bramble_message_t *o){(void)i;(void)o;return false;}
int traffic_debug_get_count(void){return 0;} int traffic_debug_get_dropped(void){return 0;} bool traffic_debug_get_event(int i, traffic_event_t *o){(void)i;(void)o;return false;} int mesh_traffic_debug_set_config(const traffic_debug_config_t *cfg){(void)cfg;return 0;} void mesh_traffic_debug_get_config(traffic_debug_config_t *cfg){memset(cfg,0,sizeof(*cfg));} bool mesh_get_traffic_debug(void){return false;}
const esp_partition_t *ota_get_running_partition(void){return 0;} int ota_wifi_start(const char *url){(void)url;return -1;}
esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out_handle){(void)ns;(void)mode;if(out_handle)*out_handle=1;return ESP_FAIL;} void nvs_close(nvs_handle_t h){(void)h;} esp_err_t nvs_get_str(nvs_handle_t h,const char* k,char* v,size_t* l){(void)h;(void)k;(void)v;(void)l;return ESP_FAIL;} esp_err_t nvs_set_str(nvs_handle_t h,const char* k,const char* v){(void)h;(void)k;(void)v;return ESP_OK;} esp_err_t nvs_set_u8(nvs_handle_t h,const char* k,uint8_t v){(void)h;(void)k;(void)v;return ESP_OK;} esp_err_t nvs_set_u16(nvs_handle_t h,const char* k,uint16_t v){(void)h;(void)k;(void)v;return ESP_OK;} esp_err_t nvs_set_u32(nvs_handle_t h,const char* k,uint32_t v){(void)h;(void)k;(void)v;return ESP_OK;} esp_err_t nvs_set_i8(nvs_handle_t h,const char* k,int8_t v){(void)h;(void)k;(void)v;return ESP_OK;} esp_err_t nvs_set_i32(nvs_handle_t h,const char* k,int32_t v){(void)h;(void)k;(void)v;return ESP_OK;} esp_err_t nvs_get_u8(nvs_handle_t h,const char* k,uint8_t *o){(void)h;(void)k;if(o)*o=0;return ESP_FAIL;} esp_err_t nvs_get_i32(nvs_handle_t h,const char* k,int32_t *o){(void)h;(void)k;if(o)*o=0;return ESP_FAIL;} esp_err_t nvs_erase_key(nvs_handle_t h,const char* k){(void)h;(void)k;return ESP_OK;} esp_err_t nvs_commit(nvs_handle_t h){(void)h;return ESP_OK;}
