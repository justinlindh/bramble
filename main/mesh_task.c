/**
 * Bramble mesh task — runs on CPU1, handles radio TX/RX and protocol dispatch.
 */

#include "mesh_task.h"
#include "util.h"
#include "broadcast_delivery_receipt.h"
#include "rpc_dispatcher.h"
#include "radio.h"
#include "packet.h"
#include "crypto.h"
#include "security.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "channel_storage.h"
#include "public_channel.h"
#include "msg_store.h"
#include "discovery.h"
#include "reliability.h"
#include "battery.h"
#include "traffic_debug.h"
#include "fragment.h"
#include "location.h"
#include "gps.h"
#include "beacon.h"
#include "delivery_event_ring.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <stdio.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdlib.h>
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <inttypes.h>

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "audio.h"
#endif

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
#include "ui_graphics.h"
#endif

static const char *TAG = "mesh";

/* Forward declarations */
static void traffic_event_notify(const traffic_event_t *evt, void *ctx);

/* ── Configuration ──────────────────────────────────────────────────── */

#define BEACON_INTERVAL_MS      60000   /* 60 seconds between beacons (A/B test) */
#define BEACON_JITTER_MS        5000    /* ±5s random jitter */
#define NEIGHBOR_PURGE_INTERVAL 60000   /* purge expired neighbors every 60s */
#define RX_QUEUE_DEPTH          16
#define MESH_EVENT_QUEUE_DEPTH  8
#define MESH_TASK_STACK         8192
#define MESH_TASK_PRIORITY      5

#define RECEIPT_QUEUE_CAPACITY  8

/* ── Received packet queue item ─────────────────────────────────────── */

typedef struct {
    uint8_t data[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t len;
    int16_t rssi;
    int8_t  snr;
} rx_packet_t;

/* ── State ──────────────────────────────────────────────────────────── */

static bramble_identity_t *s_identity;
static uint8_t             s_beacon_key[BRAMBLE_KEY_SIZE];  /* shared key for beacon HMAC */
static neighbor_table_t    s_neighbors;
static dedup_buffer_t      s_dedup;
static rreq_rate_limiter_t s_rreq_rl;
static SemaphoreHandle_t   s_state_mutex;
static SemaphoreHandle_t   s_delivery_event_mutex;
static QueueHandle_t       s_rx_queue;
static QueueHandle_t       s_mesh_event_queue;
static mesh_shared_state_t s_shared;

typedef enum {
    MESH_EVT_RECEIPT_TX = 1,
} mesh_event_type_t;

typedef struct {
    bool used;
    uint32_t original_src_addr;
    uint32_t original_packet_id;
    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    uint8_t wire_len;
    uint8_t attempts_total;
    uint8_t attempts_sent;
    uint32_t due_at_ms;
} pending_receipt_t;

static pending_receipt_t s_receipt_queue[RECEIPT_QUEUE_CAPACITY];
static esp_timer_handle_t s_receipt_timer;

static delivery_event_ring_t *s_delivery_event_ring;

enum {
    DELIVERY_EVENT_TYPE_ACK = 1,
    DELIVERY_EVENT_TYPE_BROADCAST_DELIVERY = 2,
};
static char                s_node_name[BRAMBLE_NODE_NAME_MAX + 1] = "";  /* loaded from NVS at startup */

/* Routing state */
static routing_table_t            s_routes;
static rreq_dedup_t               s_rreq_dedup;
static reverse_route_table_t      s_reverse_routes;
static pending_discovery_table_t  s_pending_disc;

/* Queued messages waiting for route discovery */
#define MAX_QUEUED_MSGS 8
typedef struct {
    uint32_t dest_addr;
    uint8_t  data[BRAMBLE_MAX_PACKET_SIZE];
    size_t   len;
    uint32_t timestamp;
    bool     used;
} queued_msg_t;
static queued_msg_t s_queued_msgs[MAX_QUEUED_MSGS];

/* Originator pseudonym lookup table for RREQ privacy.
 * Maps ephemeral pseudonyms to real addresses so incoming RREPs can be correlated.
 * Uses circular replacement when full. */
#define PSEUDONYM_TABLE_SIZE 8
typedef struct {
    uint32_t pseudonym;     /* HMAC-derived pseudonym sent in RREQ */
    uint32_t real_addr;     /* Our actual address */
    uint32_t query_id;      /* RREQ query_id (used as nonce) */
    uint32_t timestamp;     /* When this entry was created */
    bool     used;
} pseudonym_entry_t;
static pseudonym_entry_t s_pseudonym_table[PSEUDONYM_TABLE_SIZE];
static int s_pseudonym_next_slot = 0;

/* Reliability — ACK tracking for outgoing unicast messages */
static pending_ack_table_t s_pending_acks;
static airtime_budget_t    s_airtime;

/* Traffic debug telemetry */
#define TRAFFIC_DEBUG_CAPACITY 512
static traffic_event_t     s_traffic_events[TRAFFIC_DEBUG_CAPACITY];
static traffic_debug_t     s_traffic_debug;

/* Fragment reassembly context */
static reassembly_ctx_t    s_reassembly;

/* Adaptive beacon interval policy */
static beacon_policy_config_t s_beacon_policy = {
    .enabled = false,              /* Default: disabled (fixed 60s) */
    .mode = BEACON_MODE_FIXED,
    .base_interval_ms = 60000,
    .min_interval_ms = 30000,
    .max_interval_ms = 120000,
    .dense_threshold = 10,
    .churn_threshold = 3,
    .churn_window_ms = 60000,
};
static beacon_policy_status_t s_beacon_status = {
    .active_mode = BEACON_MODE_FIXED,
    .current_interval_ms = 60000,
    .neighbor_count = 0,
    .churn_events = 0,
    .last_transition_ms = 0,
    .in_backoff = false,
};
#define MAX_CHURN_HISTORY 16
typedef struct {
    uint32_t timestamp;
    uint8_t  neighbor_count;
} churn_sample_t;
static churn_sample_t s_churn_history[MAX_CHURN_HISTORY];
static int s_churn_history_idx = 0;

/* Channel state */
static bramble_channel_t   s_channels[MAX_CHANNELS];
static char                s_channel_names[MAX_CHANNELS][20];
static bool                s_channel_has_psk[MAX_CHANNELS];
static int                 s_num_channels = 0;
static int                 s_default_channel_idx = 0; /* unicast default, public broadcast stays channel 0 */
static uint32_t            s_last_broadcast_id = 0;
static broadcast_telemetry_mode_t s_broadcast_telemetry_mode = BROADCAST_TELEMETRY_RECIPIENT_ONLY;

/* Mailbox — store-and-forward for offline neighbors */
static bool s_mailbox_enabled = false;
#define MAILBOX_BEACON_FLAG 0x01
#define MAX_MAILBOX_MSGS 20
#define MAILBOX_EXPIRY_MS (3600 * 1000)  /* 1 hour */
typedef struct {
    uint32_t dest_addr;
    uint8_t  raw_pkt[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t  raw_len;
    uint32_t timestamp;
    bool     used;
} mailbox_entry_t;
static mailbox_entry_t s_mailbox[MAX_MAILBOX_MSGS];

/* Location policy engine tick state */
static uint32_t s_location_last_policy_tick_ms = 0;
static uint32_t s_location_last_send_ms = 0;
static location_manager_t s_location_mgr;

typedef struct __attribute__((packed)) {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2;
    uint32_t timestamp;
    uint32_t received_ms;
    uint8_t tier;
} persisted_peer_location_t;

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Forward declarations */
static void handle_probe(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr);
static void handle_probe_ack(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr);
static void handle_delivery_receipt(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr);
static int mesh_send_probe_round(uint32_t pid, uint8_t round);
static void mesh_start_probe_sweep(uint32_t pid);
static void mailbox_flush_for(uint32_t dest_addr);
static int transmit_packet(const uint8_t *buf, uint8_t len);
static void queue_broadcast_delivery_receipt(uint32_t original_src_addr, uint32_t original_packet_id);
static void mesh_schedule_next_receipt_timer(void);
static void mesh_process_receipt_tx_event(void);
static void mesh_receipt_timer_cb(void *arg);
static void mesh_persist_channel_psk_flags(void);
static void mesh_load_channel_psk_flags(void);
extern int location_deserialize_for_tier(const uint8_t *buf, size_t len, uint8_t tier, bramble_position_t *pos);

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void delivery_event_ring_append_locked(const delivery_event_record_t *event) {
    if (!event || !s_delivery_event_mutex || !s_delivery_event_ring) return;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    delivery_event_ring_append(s_delivery_event_ring, event);
    xSemaphoreGive(s_delivery_event_mutex);
}

static void record_ack_delivery_event(const bramble_ack_t *ack) {
    if (!ack) return;

    delivery_event_record_t evt = {0};
    evt.message_id = ack->ack_packet_id;
    evt.timestamp_s = now_ms() / 1000u;
    evt.recipient_addr = ack->src_addr;
    evt.source_addr = s_identity ? s_identity->address : 0u;
    evt.event_type = DELIVERY_EVENT_TYPE_ACK;
    evt.tier = MSG_TIER_NORMAL;

    uint8_t hops = ack->hop_count;
    if (hops > DELIVERY_EVENT_ROUTE_MAX_HOPS) hops = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    evt.route_len = hops;
    for (uint8_t i = 0; i < hops; i++) {
        evt.route_hops[i] = ack->relay_path[i];
    }

    delivery_event_ring_append_locked(&evt);
}

static void record_broadcast_delivery_event(uint32_t recipient_addr,
                                            uint32_t broadcast_id,
                                            uint8_t hop_count,
                                            const uint32_t *relay_path) {
    delivery_event_record_t evt = {0};
    evt.message_id = broadcast_id;
    evt.timestamp_s = now_ms() / 1000u;
    evt.recipient_addr = recipient_addr;
    evt.source_addr = s_identity ? s_identity->address : 0u;
    evt.event_type = DELIVERY_EVENT_TYPE_BROADCAST_DELIVERY;
    evt.tier = MSG_TIER_BROADCAST;

    uint8_t hops = hop_count;
    if (hops > DELIVERY_EVENT_ROUTE_MAX_HOPS) hops = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    evt.route_len = hops;
    if (relay_path) {
        for (uint8_t i = 0; i < hops; i++) {
            evt.route_hops[i] = relay_path[i];
        }
    }

    delivery_event_ring_append_locked(&evt);
}

static uint32_t next_packet_id(void) {
    static uint32_t counter = 0;
    if (counter == 0) {
        uint8_t buf[4];
        crypto_random(buf, 4);
        counter = (uint32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
    }
    return counter++;
}

static void location_policy_load_or_defaults(nvs_handle_t nvs, location_policy_t *policy) {
    location_policy_set_defaults(policy);

    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) {
        policy->enabled = (enabled != 0);
    }

    uint16_t interval_s = 0;
    if (nvs_get_u16(nvs, "interval_s", &interval_s) == ESP_OK) {
        policy->interval_s = interval_s;
    }

    char tier[16] = {0};
    size_t tier_len = sizeof(tier);
    if (nvs_get_str(nvs, "def_tier", tier, &tier_len) == ESP_OK) {
        policy->default_tier = location_tier_from_string(tier);
    }

    location_policy_normalize(policy);
}

static bool location_policy_has_targets(void) {
    nvs_iterator_t it = NULL;
    if (nvs_entry_find("nvs", "bramble_loc", NVS_TYPE_ANY, &it) != ESP_OK) {
        return false;
    }

    while (it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "lcr_", 4) == 0) {
            nvs_release_iterator(it);
            return true;
        }
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
    }
    nvs_release_iterator(it);
    return false;
}

uint32_t mesh_send_location_packet(uint32_t dest_addr,
                                  const bramble_position_t *pos,
                                  uint8_t tier) {
    if (!pos || !pos->valid) return 0;

    if (tier > LOCATION_TIER_PRESENCE) {
        tier = LOCATION_TIER_COARSE;
    }

    uint8_t payload[LOCATION_FULL_SIZE];
    int payload_len = location_serialize_for_tier(pos, tier, payload, sizeof(payload));
    if (payload_len <= 0) {
        return 0;
    }

    uint8_t pkt[BRAMBLE_MAX_PACKET_SIZE] = {0};
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_LOCATION,
        .flags = (uint8_t)((tier & 0x03) << FLAG_TIER_SHIFT),
        .hop_limit = 3,
        .dest_addr = dest_addr,
        .packet_id = next_packet_id(),
    };

    bramble_header_serialize(&header, pkt, HEADER_SIZE);
    memcpy(pkt + HEADER_SIZE, &s_identity->address, 4);
    memcpy(pkt + HEADER_SIZE + 4, payload, (size_t)payload_len);

    size_t wire_len = HEADER_SIZE + 4 + (size_t)payload_len;
    int rc = transmit_packet(pkt, (uint8_t)wire_len);
    if (rc == 0) {
        ESP_LOGI(TAG, "TX location packet to %08" PRIX32 " tier=%u len=%u",
                 header.dest_addr,
                 tier,
                 (unsigned)wire_len);
        return header.packet_id;
    }

    return 0;
}

static void mesh_emit_location_event(const char *event,
                                     uint32_t peer_addr,
                                     uint8_t tier,
                                     uint32_t timestamp_ms,
                                     int16_t rssi,
                                     int8_t snr,
                                     uint32_t count) {
    cJSON *params = cJSON_CreateObject();
    if (!params) return;
    cJSON_AddStringToObject(params, "event", event);
    if (peer_addr != 0) {
        char addr_buf[9];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, peer_addr);
        cJSON_AddStringToObject(params, "peer", addr_buf);
    }
    cJSON_AddNumberToObject(params, "tier", tier);
    cJSON_AddNumberToObject(params, "timestamp_ms", timestamp_ms);
    if (rssi != 0 || snr != 0) {
        cJSON_AddNumberToObject(params, "rssi", rssi);
        cJSON_AddNumberToObject(params, "snr", snr);
    }
    if (count > 0) {
        cJSON_AddNumberToObject(params, "count", count);
    }
    rpc_notify("bramble.onLocationEvent", params);
    cJSON_Delete(params);
}

static void mesh_send_location_updates(uint32_t t,
                                       const location_policy_t *policy,
                                       const bramble_position_t *source_pos) {
    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    bramble_position_t pos = *source_pos;
    pos.timestamp = t / 1000;
    pos.valid = true;

    uint32_t sent_count = 0;
    nvs_iterator_t it = NULL;
    if (nvs_entry_find("nvs", "bramble_loc", NVS_TYPE_ANY, &it) == ESP_OK) {
        while (it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            if (strncmp(info.key, "lcr_", 4) == 0) {
                const char *addr = info.key + 4;

                bool enabled = true;
                uint8_t tier = policy->default_tier;
                char raw[48] = {0};
                size_t raw_len = sizeof(raw);
                if (nvs_get_str(nvs, info.key, raw, &raw_len) == ESP_OK) {
                    int en = 1;
                    char tier_str[16] = {0};
                    int interval_tmp = 0;
                    if (sscanf(raw, "%d|%15[^|]|%d", &en, tier_str, &interval_tmp) >= 2) {
                        enabled = (en != 0);
                        tier = location_tier_from_string(tier_str);
                    }
                }

                if (enabled) {
                    uint32_t pkt_id = mesh_send_location_packet((uint32_t)strtoul(addr, NULL, 16), &pos, tier);
                    if (pkt_id != 0) {
                        sent_count++;
                    }
                }
            }

            if (nvs_entry_next(&it) != ESP_OK) {
                break;
            }
        }
        nvs_release_iterator(it);
    }

    nvs_close(nvs);

    if (sent_count > 0) {
        mesh_emit_location_event("sent", 0, policy->default_tier, t, 0, 0, sent_count);
    }
}

static void mesh_persist_peer_location(uint32_t peer_addr,
                                       const bramble_position_t *pos,
                                       uint8_t tier,
                                       uint32_t now_ms) {
    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), "lp_%08" PRIX32, peer_addr);

    persisted_peer_location_t stored = {
        .latitude_e7 = pos->latitude_e7,
        .longitude_e7 = pos->longitude_e7,
        .altitude_m = pos->altitude_m,
        .accuracy_m = pos->accuracy_m,
        .speed_kmh = pos->speed_kmh,
        .heading_deg2 = pos->heading_deg2,
        .timestamp = pos->timestamp,
        .received_ms = now_ms,
        .tier = tier,
    };

    nvs_set_blob(nvs, key, &stored, sizeof(stored));
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void handle_location(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 4 + LOCATION_PRESENCE_SIZE) {
        ESP_LOGW(TAG, "Location packet too short: %u", len);
        return;
    }

    bramble_header_t header;
    if (bramble_header_deserialize(&header, data, len) != ESP_OK) {
        return;
    }

    uint32_t src_addr = 0;
    memcpy(&src_addr, data + HEADER_SIZE, 4);
    if (src_addr == s_identity->address) {
        return;
    }

    uint8_t tier = (uint8_t)((header.flags >> FLAG_TIER_SHIFT) & 0x03);
    const uint8_t *payload = data + HEADER_SIZE + 4;
    size_t payload_len = len - HEADER_SIZE - 4;

    bramble_position_t pos = {0};
    if (location_deserialize_for_tier(payload, payload_len, tier, &pos) <= 0) {
        ESP_LOGW(TAG, "Failed to parse location payload from %08" PRIX32 " tier=%u", src_addr, tier);
        return;
    }

    uint32_t t = now_ms();
    location_cache_update(&s_location_mgr, src_addr, &pos, t);
    mesh_persist_peer_location(src_addr, &pos, tier, t);

    ESP_LOGI(TAG, "RX location from %08" PRIX32 " tier=%u RSSI:%d SNR:%d", src_addr, tier, rssi, snr);
    rpc_notify("bramble.onPeerLocation", NULL);
    mesh_emit_location_event("received", src_addr, tier, t, rssi, snr, 0);
}

static void mesh_location_policy_tick(uint32_t t) {
    const uint32_t tick_ms = 1000;
    if ((t - s_location_last_policy_tick_ms) < tick_ms) {
        return;
    }
    s_location_last_policy_tick_ms = t;

    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    location_policy_t policy;
    location_policy_load_or_defaults(nvs, &policy);

    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    bool has_manual_source = (nvs_get_i32(nvs, "lat_e6", &lat_e6) == ESP_OK) &&
                             (nvs_get_i32(nvs, "lon_e6", &lon_e6) == ESP_OK) &&
                             !(lat_e6 == 0 && lon_e6 == 0);
    nvs_close(nvs);

    bramble_position_t source_pos = {0};
    bool has_source = false;

    bramble_position_t gps_pos;
    if (gps_get_position(&gps_pos) && gps_pos.valid) {
        source_pos = gps_pos;
        has_source = true;
    } else if (has_manual_source) {
        source_pos.latitude_e7 = lat_e6 * 10;
        source_pos.longitude_e7 = lon_e6 * 10;
        source_pos.altitude_m = 0;
        source_pos.accuracy_m = 0;
        source_pos.speed_kmh = 0;
        source_pos.heading_deg2 = 0;
        source_pos.valid = true;
        has_source = true;
    }

    bool has_targets = location_policy_has_targets();

    if (location_policy_should_send(&policy, has_source, has_targets, t, s_location_last_send_ms)) {
        mesh_send_location_updates(t, &policy, &source_pos);
        s_location_last_send_ms = t;
    }
}

static void mesh_persist_channel_psk_flags(void) {
    nvs_handle_t h;
    if (nvs_open("bramble_ch", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < MAX_CHANNELS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "psk%d", i);
        if (i < s_num_channels) {
            nvs_set_u8(h, key, s_channel_has_psk[i] ? 1 : 0);
        } else {
            nvs_erase_key(h, key);
        }
    }

    nvs_commit(h);
    nvs_close(h);
}

static void mesh_load_channel_psk_flags(void) {
    nvs_handle_t h;
    if (nvs_open("bramble_ch", NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < s_num_channels; i++) {
        /* Missing metadata defaults to "no PSK lock" for deterministic export semantics. */
        uint8_t has_psk = 0;
        char key[8];
        snprintf(key, sizeof(key), "psk%d", i);
        if (nvs_get_u8(h, key, &has_psk) != ESP_OK) {
            has_psk = 0;
        }
        s_channel_has_psk[i] = (has_psk != 0);
    }

    nvs_close(h);
}

/* ── Reboot timer ───────────────────────────────────────────────────── */

static void reboot_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    ESP_LOGI(TAG, "Rebooting (requested via RPC)...");
    esp_restart();
}

void mesh_reboot_delayed(int delay_ms) {
    if (delay_ms <= 0) delay_ms = 100;
    TimerHandle_t t = xTimerCreate("reboot", pdMS_TO_TICKS(delay_ms),
                                   pdFALSE, NULL, reboot_timer_cb);
    if (t == NULL) {
        ESP_LOGE(TAG, "Failed to create reboot timer — rebooting immediately");
        esp_restart();
        return;
    }
    if (xTimerStart(t, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start reboot timer — rebooting immediately");
        esp_restart();
    } else {
        ESP_LOGI(TAG, "Reboot scheduled in %d ms", delay_ms);
    }
}

/* ── Radio callbacks (ISR context → queue) ──────────────────────────── */

static void on_rx(const uint8_t *data, uint8_t len, const radio_rx_info_t *info) {
    rx_packet_t pkt;
    /* len is uint8_t (max 255), pkt.data is 256 bytes — always fits */
    memcpy(pkt.data, data, len);
    pkt.len = len;
    pkt.rssi = info->rssi;
    pkt.snr = info->snr;

    /* Called from radio task context, not ISR */
    if (xQueueSend(s_rx_queue, &pkt, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, dropping packet");
    }
}

static void on_tx_done(void) {
    ESP_LOGD(TAG, "TX complete");
}

/* ── Beacon TX ──────────────────────────────────────────────────────── */

static int send_beacon(void) {
    bramble_beacon_t beacon = {0};

    beacon.header.version = BRAMBLE_VERSION;
    beacon.header.type = PKT_TYPE_BEACON;
    beacon.header.flags = 0;
    beacon.header.hop_limit = 1;  /* beacons are 1-hop only */
    beacon.header.dest_addr = 0xFFFFFFFF;  /* broadcast */
    beacon.header.packet_id = next_packet_id();

    beacon.src_addr = s_identity->address;
    beacon.pubkey_hash = s_identity->pubkey_hash;
    beacon.uptime_min = (uint16_t)(now_ms() / 60000);
    beacon.battery_pct = battery_read_pct();
    beacon.tx_queue_depth = 0;
    beacon.neighbor_count = (uint8_t)neighbor_count(&s_neighbors);
    beacon.flags = s_mailbox_enabled ? MAILBOX_BEACON_FLAG : 0;
    /* TODO: timesync integration deferred — requires global timesync_state_t 
     * and bidirectional packet flow. See components/timesync for implementation. */
    beacon.network_time = 0;
    beacon.time_confidence = 0xFFFF;  /* no confidence */

    /* Include node name in beacon (if set) */
    if (s_node_name[0] != '\0') {
        beacon.name_len = (uint8_t)strlen(s_node_name);
        if (beacon.name_len > BEACON_NAME_MAX) beacon.name_len = BEACON_NAME_MAX;
        memcpy(beacon.name, s_node_name, beacon.name_len);
        beacon.name[beacon.name_len] = '\0';
    }

    /* HMAC auth — use shared beacon key (derived from public channel PSK) */
    beacon_compute_hmac(&beacon, s_beacon_key, sizeof(s_beacon_key));

    uint8_t buf[64];
    if (bramble_beacon_serialize(&beacon, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Beacon serialize failed");
        return -1;
    }

    size_t beacon_wire_len = bramble_beacon_wire_size(&beacon);
    int ret = transmit_packet(buf, (uint8_t)beacon_wire_len);
    if (ret == 0) {
        uint32_t airtime_est = 30 + (uint32_t)(beacon_wire_len * 4);
        uint32_t t_now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        airtime_budget_set_mesh_size(&s_airtime, (uint8_t)neighbor_count(&s_neighbors));
        airtime_budget_refill(&s_airtime, t_now);
        airtime_budget_debit(&s_airtime, AIRTIME_TIER_BROADCAST, airtime_est);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.beacon_tx_count++;
        s_shared.packets_tx++;
        s_shared.airtime = s_airtime;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Beacon TX #%" PRIu32 " (neighbors: %d)",
                 s_shared.beacon_tx_count, neighbor_count(&s_neighbors));
    } else {
        ESP_LOGE(TAG, "Beacon TX failed: %d", ret);
    }
    return ret;
}

/* ── Packet handlers ────────────────────────────────────────────────── */

static void handle_beacon(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid beacon (len=%u)", len);
        return;
    }

    /* Ignore our own beacons */
    if (beacon.src_addr == s_identity->address) return;

    /* Verify beacon HMAC authenticity using shared beacon key */
    if (!beacon_verify_hmac(&beacon, s_beacon_key, sizeof(s_beacon_key))) {
        ESP_LOGW(TAG, "Beacon from %08" PRIX32 " failed HMAC verification, discarding",
                 beacon.src_addr);
        return;
    }

    /* Check for address collision — different pubkey_hash but same address */
    if (identity_check_collision(s_identity, beacon.src_addr, beacon.pubkey_hash)) {
        ESP_LOGE(TAG, "ADDRESS COLLISION with %08" PRIX32 " — regenerating identity!", beacon.src_addr);
        /* Regenerate keypair and persist to NVS */
        identity_generate_and_save(s_identity);
        ESP_LOGW(TAG, "New identity: %08" PRIX32, s_identity->address);
        /* Notify webapp */
        cJSON *params = cJSON_CreateObject();
        char addr_buf[12];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, s_identity->address);
        cJSON_AddStringToObject(params, "new_address", addr_buf);
        cJSON_AddStringToObject(params, "reason", "address_collision");
        rpc_notify("bramble.onIdentityChange", params);
        cJSON_Delete(params);
        return;
    }

    /* Update neighbor table — track if this is a new neighbor */
    uint32_t t = now_ms();
    int old_count = neighbor_count(&s_neighbors);
    int idx = neighbor_update(&s_neighbors, beacon.src_addr, (int8_t)rssi, snr,
                              beacon.pubkey_hash, t);
    int new_count = neighbor_count(&s_neighbors);
    bool is_new_peer = (new_count > old_count);
    
    /* Store peer name if present */
    if (idx >= 0 && beacon.name_len > 0) {
        memcpy(s_neighbors.entries[idx].name, beacon.name, beacon.name_len);
        s_neighbors.entries[idx].name[beacon.name_len] = '\0';
    } else if (idx >= 0) {
        s_neighbors.entries[idx].name[0] = '\0';
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.beacon_rx_count++;
    s_shared.last_rx_rssi = rssi;
    s_shared.last_rx_snr = snr;
    s_shared.neighbors = s_neighbors;
    xSemaphoreGive(s_state_mutex);

    if (idx >= 0) {
        ESP_LOGI(TAG, "Neighbor %08" PRIX32 " RSSI:%d SNR:%d (total: %d)%s",
                 beacon.src_addr, rssi, snr, neighbor_count(&s_neighbors),
                 is_new_peer ? " [NEW]" : "");

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        /* Play peer join tone for new neighbors */
        if (is_new_peer && audio_is_available()) {
            audio_play_tone(AUDIO_TONE_PEER_JOIN);
        }
#endif

        /* Mailbox: flush any stored messages for this newly-seen neighbor */
        if (s_mailbox_enabled) {
            mailbox_flush_for(beacon.src_addr);
        }
    }

    /* Sybil detection — check if multiple neighbors cluster at suspiciously similar RSSI.
     * Log-only for now; detection algorithm needs field validation before dropping beacons. */
    {
        int nc = neighbor_count(&s_neighbors);
        if (nc >= 3) {
            int8_t rssi_vals[MAX_NEIGHBORS];
            for (int i = 0; i < nc && i < MAX_NEIGHBORS; i++) {
                rssi_vals[i] = s_neighbors.entries[i].rssi;
            }
            if (sybil_check_rssi_cluster(rssi_vals, nc)) {
                ESP_LOGW(TAG, "SYBIL WARNING: beacon from %08" PRIX32
                         " — %d neighbors with suspiciously similar RSSI (latest RSSI:%d)",
                         beacon.src_addr, nc, rssi);
            }
        }
    }

    /* Notify any RPC clients that the neighbor table changed */
    rpc_notify("bramble.onNeighborChange", NULL);
}

/* ── ACK handling ────────────────────────────────────────────────────── */

static void mesh_schedule_next_receipt_timer(void) {
    if (!s_receipt_timer) return;

    uint32_t t = now_ms();
    uint32_t earliest_due = 0;
    bool have_pending = false;

    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used) continue;
        if (!have_pending || s_receipt_queue[i].due_at_ms < earliest_due) {
            earliest_due = s_receipt_queue[i].due_at_ms;
            have_pending = true;
        }
    }

    if (!have_pending) {
        esp_timer_stop(s_receipt_timer);
        return;
    }

    uint32_t delay_ms = (earliest_due <= t) ? 1u : (earliest_due - t);
    esp_timer_stop(s_receipt_timer);
    esp_err_t err = esp_timer_start_once(s_receipt_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm receipt timer: %d", (int)err);
    }
}

static void queue_broadcast_delivery_receipt(uint32_t original_src_addr, uint32_t original_packet_id) {
    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    size_t wire_len = 0;

    /* Determine receipt policy based on mesh size */
    uint8_t policy = mesh_broadcast_receipt_policy(0xFFFFFFFFu,
                         (uint8_t)neighbor_count(&s_neighbors));
    uint8_t hop_limit = (policy >= 2) ? 8 : 1;  /* full=8, neighbors-only=1 */

    esp_err_t err = mesh_build_broadcast_delivery_receipt_packet(s_identity->address,
                                                                  next_packet_id(),
                                                                  original_src_addr,
                                                                  original_packet_id,
                                                                  hop_limit,
                                                                  buf,
                                                                  sizeof(buf),
                                                                  &wire_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Delivery receipt build failed: %d", (int)err);
        return;
    }

    int slot = -1;
    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "Delivery receipt queue full; dropping pkt=%08" PRIX32, original_packet_id);
        return;
    }

    uint32_t slot_delay_ms = mesh_broadcast_receipt_slot_delay_ms(s_identity->address, original_packet_id);
    uint32_t initial_delay_ms = slot_delay_ms + (esp_random() % 140u); /* +0..139ms */

    pending_receipt_t *item = &s_receipt_queue[slot];
    memset(item, 0, sizeof(*item));
    item->used = true;
    item->original_src_addr = original_src_addr;
    item->original_packet_id = original_packet_id;
    memcpy(item->buf, buf, wire_len);
    item->wire_len = (uint8_t)wire_len;
    item->attempts_total = mesh_broadcast_receipt_retry_count();
    if (item->attempts_total == 0) {
        item->attempts_total = 1;
    }
    item->attempts_sent = 0;
    item->due_at_ms = now_ms() + initial_delay_ms;

    mesh_schedule_next_receipt_timer();
}

static void mesh_process_receipt_tx_event(void) {
    uint32_t t_now = now_ms();
    int due_idx = -1;

    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used) continue;
        if (s_receipt_queue[i].due_at_ms <= t_now) {
            due_idx = i;
            break;
        }
    }

    if (due_idx < 0) {
        mesh_schedule_next_receipt_timer();
        return;
    }

    pending_receipt_t *item = &s_receipt_queue[due_idx];
    uint8_t attempt_no = (uint8_t)(item->attempts_sent + 1u);
    uint32_t airtime_est = 30u + (uint32_t)(item->wire_len * 4u);

    airtime_budget_set_mesh_size(&s_airtime, (uint8_t)neighbor_count(&s_neighbors));
    airtime_budget_refill(&s_airtime, t_now);
    if (!airtime_budget_can_transmit(&s_airtime, AIRTIME_TIER_RECEIPT, airtime_est)) {
        ESP_LOGW(TAG,
                 "Delivery receipt suppressed for pkt=%08" PRIX32 " (attempt=%u/%u): receipt airtime budget exhausted",
                 item->original_packet_id,
                 (unsigned)attempt_no,
                 (unsigned)item->attempts_total);
        memset(item, 0, sizeof(*item));
        mesh_schedule_next_receipt_timer();
        return;
    }

    /* TX path can block for CAD/LBT + radio wait; feed task WDT just before entering it. */
    esp_task_wdt_reset();

    if (transmit_packet(item->buf, item->wire_len) == 0) {
        airtime_budget_debit(&s_airtime, AIRTIME_TIER_RECEIPT, airtime_est);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.airtime = s_airtime;
        xSemaphoreGive(s_state_mutex);

        ESP_LOGI(TAG,
                 "TX delivery receipt for broadcast pkt=%08" PRIX32 " to %08" PRIX32
                 " attempt=%u/%u",
                 item->original_packet_id,
                 item->original_src_addr,
                 (unsigned)attempt_no,
                 (unsigned)item->attempts_total);
    }

    item->attempts_sent++;
    if (item->attempts_sent >= item->attempts_total) {
        memset(item, 0, sizeof(*item));
        mesh_schedule_next_receipt_timer();
        return;
    }

    uint8_t i = (uint8_t)(item->attempts_sent - 1u);
    uint32_t base_ms = 500u + ((uint32_t)i * 700u);
    uint32_t jitter_range = 500u + ((uint32_t)i * 400u);
    uint32_t retry_delay_ms = base_ms + (esp_random() % jitter_range);
    item->due_at_ms = now_ms() + retry_delay_ms;

    mesh_schedule_next_receipt_timer();
}

static void mesh_receipt_timer_cb(void *arg) {
    (void)arg;
    if (!s_mesh_event_queue) return;

    mesh_event_type_t evt = MESH_EVT_RECEIPT_TX;
    if (xQueueSend(s_mesh_event_queue, &evt, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "mesh event queue full; dropped receipt timer event");
    }
}

static void send_ack(uint32_t dest_addr, uint32_t ack_packet_id, int8_t rssi) {
    bramble_ack_t ack = {
        .header = {
            .version = BRAMBLE_VERSION,
            .type = PKT_TYPE_ACK,
            .flags = 0,
            .hop_limit = 8,
            .dest_addr = dest_addr,
            .packet_id = next_packet_id(),
        },
        .src_addr = s_identity->address,
        .ack_packet_id = ack_packet_id,
        .ack_flags = 0,
        .rssi_at_dest = rssi,
        .hop_count = 1,
        .relay_path = { s_identity->address },  /* destination is first hop */
    };

    uint8_t buf[64];
    esp_err_t err = bramble_ack_serialize(&ack, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ACK serialize failed");
        return;
    }
    size_t wire_len = bramble_ack_wire_size(&ack);
    int ret = transmit_packet(buf, (uint8_t)wire_len);
    if (ret == 0) {
        ESP_LOGI(TAG, "ACK sent for pkt %08" PRIX32 " to %08" PRIX32 " (%u hops)",
                 ack_packet_id, dest_addr, ack.hop_count);
    }
}

static void forward_ack(bramble_ack_t *ack, int16_t rssi) {
    /* Append our address to the relay path */
    if (ack->hop_count < ACK_MAX_HOPS) {
        ack->relay_path[ack->hop_count++] = s_identity->address;
    }

    /* Decrement hop limit */
    if (ack->header.hop_limit <= 1) {
        ESP_LOGD(TAG, "ACK hop limit reached, dropping");
        return;
    }
    ack->header.hop_limit--;

    /* Look up route back to the original sender */
    route_entry_t *route = route_lookup(&s_routes, ack->header.dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        ESP_LOGW(TAG, "No route to forward ACK to %08" PRIX32, ack->header.dest_addr);
        return;
    }

    uint8_t buf[64];
    esp_err_t err = bramble_ack_serialize(ack, buf, sizeof(buf));
    if (err != ESP_OK) return;

    size_t wire_len = bramble_ack_wire_size(ack);
    ESP_LOGI(TAG, "Forwarding ACK for pkt %08" PRIX32 " toward %08" PRIX32 " (%u hops)",
             ack->ack_packet_id, ack->header.dest_addr, ack->hop_count);
    transmit_packet(buf, (uint8_t)wire_len);
}

static void handle_ack(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_ack_t ack;
    esp_err_t err = bramble_ack_deserialize(&ack, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ACK deserialize failed");
        return;
    }

    /* Not for us — forward it */
    if (ack.header.dest_addr != s_identity->address) {
        forward_ack(&ack, rssi);
        return;
    }

    ESP_LOGI(TAG, "ACK received for pkt %08" PRIX32 " from %08" PRIX32 " (RSSI at dest: %d, %u hops)",
             ack.ack_packet_id, ack.src_addr, ack.rssi_at_dest, ack.hop_count);

    /* Remove from pending ACK table */
    bool found = pending_ack_remove(&s_pending_acks, ack.ack_packet_id);

    uint32_t route_hops[MSG_ROUTE_MAX_HOPS] = {0};
    uint8_t route_hop_count = 0;

    /* Relay path from ACK is dest→...→sender; normalize to sender→...→dest for UIs. */
    if (s_identity) {
        route_hops[route_hop_count++] = s_identity->address;
    }
    for (int i = ack.hop_count - 1; i >= 0 && route_hop_count < MSG_ROUTE_MAX_HOPS; i--) {
        route_hops[route_hop_count++] = ack.relay_path[i];
    }

    /* Update message store status */
    if (msg_store_update_status_with_route(ack.ack_packet_id, MSG_STATUS_DELIVERED, route_hop_count, route_hops)) {
        record_ack_delivery_event(&ack);
        /* Notify webapp with full relay path from ACK */
        char addr_buf[12];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, ack.src_addr);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "from", addr_buf);
        char pkt_buf[12];
        snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, ack.ack_packet_id);
        cJSON_AddStringToObject(params, "packet_id", pkt_buf);
        cJSON_AddStringToObject(params, "status", "delivered");
        cJSON_AddNumberToObject(params, "rssi_at_dest", ack.rssi_at_dest);

        cJSON *path = cJSON_AddArrayToObject(params, "relayPath");
        char hop_buf[12];
        for (uint8_t i = 0; i < route_hop_count; i++) {
            cJSON *hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, route_hops[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            cJSON_AddNumberToObject(hop, "rssi", (i == (route_hop_count - 1)) ? ack.rssi_at_dest : 0);
            cJSON_AddItemToArray(path, hop);
        }

        rpc_notify("bramble.onAck", params);
        cJSON_Delete(params);
    }

    if (found) {
        ESP_LOGI(TAG, "Message delivered to %08" PRIX32, ack.src_addr);
    }
}

static void forward_delivery_receipt(bramble_delivery_receipt_t *receipt) {
    if (!receipt) return;

    if (receipt->hop_count < DELIVERY_RECEIPT_MAX_HOPS) {
        receipt->relay_path[receipt->hop_count++] = s_identity->address;
    }

    if (receipt->header.hop_limit <= 1) {
        ESP_LOGD(TAG, "Delivery receipt hop limit reached, dropping");
        return;
    }
    receipt->header.hop_limit--;

    route_entry_t *route = route_lookup(&s_routes, receipt->header.dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        ESP_LOGW(TAG, "No route to forward delivery receipt to %08" PRIX32, receipt->header.dest_addr);
        return;
    }

    uint8_t buf[96];
    esp_err_t err = bramble_delivery_receipt_serialize(receipt, buf, sizeof(buf));
    if (err != ESP_OK) return;

    size_t wire_len = DELIVERY_RECEIPT_MIN_SIZE + ((size_t)receipt->hop_count * 4u);
    uint32_t airtime_est = 30u + (uint32_t)(wire_len * 4u);
    uint32_t t_now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    airtime_budget_set_mesh_size(&s_airtime, (uint8_t)neighbor_count(&s_neighbors));
    airtime_budget_refill(&s_airtime, t_now);
    if (!airtime_budget_can_transmit(&s_airtime, AIRTIME_TIER_RECEIPT, airtime_est)) {
        ESP_LOGW(TAG,
                 "Forwarded delivery receipt suppressed for pkt=%08" PRIX32 ": receipt airtime budget exhausted",
                 receipt->orig_packet_id);
        return;
    }

    ESP_LOGI(TAG, "Forwarding delivery receipt for pkt %08" PRIX32 " toward %08" PRIX32 " (%u hops)",
             receipt->orig_packet_id, receipt->header.dest_addr, receipt->hop_count);
    if (transmit_packet(buf, (uint8_t)wire_len) == 0) {
        airtime_budget_debit(&s_airtime, AIRTIME_TIER_RECEIPT, airtime_est);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.airtime = s_airtime;
        xSemaphoreGive(s_state_mutex);
    }
}

static void handle_delivery_receipt(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    (void)snr;
    bramble_delivery_receipt_t receipt;
    if (bramble_delivery_receipt_deserialize(&receipt, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Delivery receipt deserialize failed");
        return;
    }

    if (receipt.header.dest_addr != s_identity->address) {
        forward_delivery_receipt(&receipt);
        return;
    }

    mesh_emit_broadcast_delivery_notification(receipt.src_addr,
                                              receipt.orig_packet_id,
                                              rssi,
                                              receipt.hop_count,
                                              receipt.relay_path);
}

static void handle_data(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    /* Data packet layout: header(12) + src_addr(4) + nonce(12) + ciphertext(N) + tag(16) */
    if (len < HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE + 1) {
        ESP_LOGW(TAG, "Data packet too short: %u", len);
        return;
    }

    uint32_t src_addr;
    memcpy(&src_addr, data + HEADER_SIZE, 4);

    const uint8_t *nonce = data + HEADER_SIZE + 4;
    size_t ct_len = len - HEADER_SIZE - 4 - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;
    const uint8_t *ciphertext = nonce + BRAMBLE_NONCE_SIZE;
    const uint8_t *tag = ciphertext + ct_len;

    /* Try to decrypt with known channels */
    channel_msg_info_t info;
    if (ct_len > BRAMBLE_MAX_PACKET_SIZE) {
        ESP_LOGW(TAG, "Data too large: %u", (unsigned)ct_len);
        return;
    }

    int ret = channel_msg_decrypt(s_channels, s_num_channels,
                                  nonce, ciphertext, ct_len, tag, &info);
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to decrypt data from %08" PRIX32, src_addr);
        return;
    }

    /* Extract the text message from the decrypted payload */
    if (info.data_len > 0) {
        /* Check if this is a fragment */
        if (info.data_len >= FRAG_HEADER_SIZE) {
            frag_header_t frag_hdr;
            frag_hdr.frag_index = info.data[0];
            frag_hdr.frag_total = info.data[1];
            frag_hdr.message_id = info.data[2] | ((uint16_t)info.data[3] << 8);

            /* Validate fragment header — indices < total and total within limits */
            if (frag_hdr.frag_total > 1 && frag_hdr.frag_index < frag_hdr.frag_total &&
                frag_hdr.frag_total <= FRAG_MAX_FRAGMENTS) {
                /* This is a fragment — process through reassembly */
                ESP_LOGI(TAG, "RX fragment %u/%u msg_id=%04X from %08" PRIX32,
                         frag_hdr.frag_index + 1, frag_hdr.frag_total,
                         frag_hdr.message_id, info.src_addr);

                int ret = reassembly_add(&s_reassembly, &frag_hdr,
                                        info.data + FRAG_HEADER_SIZE,
                                        info.data_len - FRAG_HEADER_SIZE,
                                        now_ms());
                if (ret == 1) {
                    /* Reassembly complete — collect the full message.
                     * Buffers allocated on heap to avoid ~1.2KB stack pressure
                     * in the mesh task (which has tight stack headroom). */
                    size_t reasm_sz = FRAG_MAX_FRAGMENTS * FRAG_MAX_PLAINTEXT;
                    uint8_t *reassembled = malloc(reasm_sz);
                    if (!reassembled) {
                        ESP_LOGE(TAG, "OOM for reassembly buffer");
                        return;
                    }
                    int total_len = reassembly_collect(&s_reassembly, frag_hdr.message_id,
                                                       reassembled, reasm_sz);
                    if (total_len > 0) {
                        /* Process the reassembled message */
                        char *text = malloc(reasm_sz + 1);
                        if (!text) {
                            ESP_LOGE(TAG, "OOM for reassembly text buffer");
                            free(reassembled);
                            return;
                        }
                        size_t tlen = (size_t)total_len;
                        if (tlen >= reasm_sz + 1) tlen = reasm_sz;
                        memcpy(text, reassembled, tlen);
                        text[tlen] = '\0';

                        ESP_LOGI(TAG, "");
                        ESP_LOGI(TAG, "*** REASSEMBLED MESSAGE from %08" PRIX32 " ***", info.src_addr);
                        ESP_LOGI(TAG, ">>> %s", text);
                        ESP_LOGI(TAG, "*** (%u bytes from %u fragments, ch:%d RSSI:%d SNR:%d) ***",
                                 (unsigned)total_len, frag_hdr.frag_total, info.channel_id, rssi, snr);

                        /* Store and notify for reassembled message */
                        uint32_t hdr_dest;
                        memcpy(&hdr_dest, data + 4, 4);
                        bool is_channel_message = (info.channel_id > 0);
                        msg_direction_t dir = (hdr_dest == 0xFFFFFFFF && !is_channel_message)
                            ? MSG_DIR_BROADCAST_IN : MSG_DIR_INCOMING;
                        int16_t channel_index = (int16_t)info.channel_id;
                        msg_store_add_ex2(info.src_addr, dir, text, tlen, rssi, snr,
                                          0, MSG_STATUS_NONE, channel_index);

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
                        ui_graphics_notify(UI_EVT_MSG_RECEIVED);
#endif

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
                        if (audio_is_available()) {
                            audio_play_tone(AUDIO_TONE_MESSAGE_RX);
                        }
#endif

                        /* Emit onMessage notification via RPC */
                        {
                            char addr_buf[12];
                            snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, info.src_addr);

                            cJSON *params = cJSON_CreateObject();
                            cJSON_AddStringToObject(params, "from", addr_buf);
                            cJSON_AddStringToObject(params, "text", text);
                            cJSON_AddNumberToObject(params, "rssi", rssi);
                            cJSON_AddNumberToObject(params, "snr", snr);
                            cJSON_AddNumberToObject(params, "channel", (info.channel_id > 0) ? info.channel_id : -1);
                            cJSON_AddBoolToObject(params, "broadcast", (dir == MSG_DIR_BROADCAST_IN));
                            rpc_notify("bramble.onMessage", params);
                            cJSON_Delete(params);
                        }

                        bramble_header_t rx_hdr;
                        bramble_header_deserialize(&rx_hdr, data, len);

                        /* Send ACK for unicast messages */
                        if (dir == MSG_DIR_INCOMING) {
                            send_ack(info.src_addr, rx_hdr.packet_id, rssi);
                        } else if (mesh_should_emit_broadcast_delivery_receipt(rx_hdr.dest_addr,
                                       (uint8_t)neighbor_count(&s_neighbors))) {
                            queue_broadcast_delivery_receipt(info.src_addr, rx_hdr.packet_id);
                        }

                        /* Print to stdout */
                        printf("\n[MSG from %08" PRIX32 "] %s\n", info.src_addr, text);
                        printf("bramble> ");
                        fflush(stdout);
                        free(text);
                    } else {
                        ESP_LOGW(TAG, "Failed to collect reassembled message");
                    }
                    free(reassembled);
                } else if (ret < 0) {
                    ESP_LOGW(TAG, "Fragment reassembly failed");
                }
                return; /* Fragment processed, don't treat as regular message */
            }
        }

        /* Not a fragment — process as regular single-packet message */
        char text[256];
        size_t tlen = info.data_len;
        if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
        memcpy(text, info.data, tlen);
        text[tlen] = '\0';

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "*** MESSAGE from %08" PRIX32 " ***", info.src_addr);
        ESP_LOGI(TAG, ">>> %s", text);
        ESP_LOGI(TAG, "*** (ch:%d RSSI:%d SNR:%d) ***", info.channel_id, rssi, snr);

        /* Store in message store — classify broadcast vs channel routing */
        uint32_t hdr_dest;
        memcpy(&hdr_dest, data + 4, 4);  /* dest_addr at offset 4 in header */
        bool is_channel_message = (info.channel_id > 0);
        msg_direction_t dir = (hdr_dest == 0xFFFFFFFF && !is_channel_message)
            ? MSG_DIR_BROADCAST_IN : MSG_DIR_INCOMING;
        int16_t channel_index = (int16_t)info.channel_id;
        msg_store_add_ex2(info.src_addr, dir, text, tlen, rssi, snr,
                          0, MSG_STATUS_NONE, channel_index);

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
        /* Notify UI of new message for unread badge and refresh */
        ui_graphics_notify(UI_EVT_MSG_RECEIVED);
#endif

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        /* Play message received tone */
        if (audio_is_available()) {
            audio_play_tone(AUDIO_TONE_MESSAGE_RX);
        }
#endif

        /* Emit onMessage notification via RPC */
        {
            char addr_buf[12];
            snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, info.src_addr);

            cJSON *params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "from", addr_buf);
            cJSON_AddStringToObject(params, "text", text);
            cJSON_AddNumberToObject(params, "rssi", rssi);
            cJSON_AddNumberToObject(params, "snr", snr);
            cJSON_AddNumberToObject(params, "channel", (info.channel_id > 0) ? info.channel_id : -1);
            cJSON_AddBoolToObject(params, "broadcast",
                dir == MSG_DIR_BROADCAST_IN);
            rpc_notify("bramble.onMessage", params);
            cJSON_Delete(params);
        }

        /* Deserialize packet_id from header for response telemetry */
        bramble_header_t rx_hdr;
        bramble_header_deserialize(&rx_hdr, data, len);

        /* Send ACK for unicast messages (not broadcasts) */
        if (dir == MSG_DIR_INCOMING) {
            send_ack(info.src_addr, rx_hdr.packet_id, rssi);
        } else if (mesh_should_emit_broadcast_delivery_receipt(rx_hdr.dest_addr,
                       (uint8_t)neighbor_count(&s_neighbors))) {
            queue_broadcast_delivery_receipt(info.src_addr, rx_hdr.packet_id);
        }

        /* Also print to stdout for CLI users */
        printf("\n[MSG from %08" PRIX32 "] %s\n", info.src_addr, text);
        printf("bramble> ");
        fflush(stdout);
    }
}

/* ── Routing packet handlers ────────────────────────────────────────── */

#define LBT_MAX_ATTEMPTS     3u
#define LBT_BACKOFF_BASE_MS  50u
#define LBT_BACKOFF_MAX_MS   300u

static int transmit_packet(const uint8_t *buf, uint8_t len) {
    /* Extract packet type for telemetry (assumes header is already serialized) */
    uint8_t pkt_type = (len >= 2) ? buf[1] : 0xFF;

    /* Extract tier from flags (bits 6-7) */
    uint8_t flags = (len >= 3) ? buf[2] : 0;
    uint8_t tier = ((flags >> FLAG_TIER_SHIFT) & 0x03);
    if (tier == 0) tier = 0x01; /* default to normal if not set */

    /* Listen-Before-Talk: check channel before transmitting */
    for (uint8_t attempt = 0; attempt < LBT_MAX_ATTEMPTS; attempt++) {
        esp_task_wdt_reset();
        if (!radio_cad_check()) {
            break; /* Channel is clear */
        }

        /* Channel busy — back off with randomized exponential delay */
        uint32_t backoff_ms = LBT_BACKOFF_BASE_MS * (1u << attempt);
        if (backoff_ms > LBT_BACKOFF_MAX_MS) {
            backoff_ms = LBT_BACKOFF_MAX_MS;
        }
        backoff_ms += (esp_random() % backoff_ms);
        ESP_LOGD(TAG, "LBT: channel busy (attempt %u/%u), backoff %" PRIu32 "ms",
                 (unsigned)(attempt + 1), LBT_MAX_ATTEMPTS, backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
    /* After LBT_MAX_ATTEMPTS, transmit anyway to avoid starvation */

    esp_task_wdt_reset();
    int ret = radio_transmit(buf, len);
    if (ret == 0) {
        /* Record successful TX */
        traffic_debug_record_tx(&s_traffic_debug, pkt_type, len, tier);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.packets_tx++;
        xSemaphoreGive(s_state_mutex);
    }
    return ret;
}

static void send_rreq(const bramble_rreq_t *rreq) {
    uint8_t buf[64];
    if (bramble_rreq_serialize(rreq, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RREQ query=%08" PRIX32 " dest=%08" PRIX32,
                 rreq->query_id, rreq->header.dest_addr);
        transmit_packet(buf, HEADER_SIZE + 18); /* RREQ payload = 18 bytes */
    }
}

static void send_rrep(const bramble_rrep_t *rrep) {
    uint8_t buf[64];
    if (bramble_rrep_serialize(rrep, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RREP query=%08" PRIX32 " → next=%08" PRIX32,
                 rrep->query_id, rrep->next_hop);
        transmit_packet(buf, HEADER_SIZE + 19); /* RREP payload = 19 bytes */
    }
}

static void send_rerr(uint32_t broken_dest, uint32_t broken_next_hop) {
    bramble_rerr_t rerr = {
        .header = {
            .version = BRAMBLE_VERSION,
            .type = PKT_TYPE_RERR,
            .flags = 0,
            .hop_limit = 3,
            .dest_addr = 0xFFFFFFFF, /* broadcast */
            .packet_id = next_packet_id(),
        },
        .reporter_addr = s_identity->address,
        .broken_dest = broken_dest,
        .broken_next_hop = broken_next_hop,
    };
    uint8_t buf[64];
    if (bramble_rerr_serialize(&rerr, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RERR broken_dest=%08" PRIX32, broken_dest);
        transmit_packet(buf, HEADER_SIZE + 12);
    }
}

/* ── Originator pseudonym helpers for RREQ privacy ─────────────────── */

/**
 * Generate an ephemeral pseudonym for RREQ originator privacy.
 * Pseudonym = HMAC-SHA256(private_key, address || query_id)[0..3]
 * The query_id acts as a nonce, ensuring a different pseudonym per RREQ.
 */
static uint32_t pseudonym_generate(const uint8_t *private_key, uint32_t address, uint32_t query_id) {
    uint8_t input[8];
    memcpy(input, &address, 4);
    memcpy(input + 4, &query_id, 4);

    uint8_t hmac_out[32];
    crypto_hmac_sha256(private_key, BRAMBLE_KEY_SIZE, input, sizeof(input), hmac_out);

    /* Truncate to 4 bytes (same size as Bramble address) */
    uint32_t pseudonym;
    memcpy(&pseudonym, hmac_out, 4);
    return pseudonym;
}

/**
 * Store a pseudonym mapping in the lookup table.
 * Uses circular replacement when table is full.
 */
static void pseudonym_store(uint32_t pseudonym, uint32_t real_addr, uint32_t query_id, uint32_t timestamp) {
    pseudonym_entry_t *entry = &s_pseudonym_table[s_pseudonym_next_slot];
    entry->pseudonym = pseudonym;
    entry->real_addr = real_addr;
    entry->query_id = query_id;
    entry->timestamp = timestamp;
    entry->used = true;

    s_pseudonym_next_slot = (s_pseudonym_next_slot + 1) % PSEUDONYM_TABLE_SIZE;

    ESP_LOGD(TAG, "Pseudonym stored: %08" PRIX32 " → addr=%08" PRIX32 " query=%08" PRIX32,
             pseudonym, real_addr, query_id);
}

/**
 * Look up a pseudonym in the table.
 * Returns the entry if found, NULL otherwise.
 */
static pseudonym_entry_t *pseudonym_lookup(uint32_t pseudonym) {
    for (int i = 0; i < PSEUDONYM_TABLE_SIZE; i++) {
        if (s_pseudonym_table[i].used && s_pseudonym_table[i].pseudonym == pseudonym) {
            return &s_pseudonym_table[i];
        }
    }
    return NULL;
}

/**
 * Look up a pseudonym by query_id.
 * Returns the entry if found, NULL otherwise.
 */
static pseudonym_entry_t *pseudonym_lookup_by_query(uint32_t query_id) {
    for (int i = 0; i < PSEUDONYM_TABLE_SIZE; i++) {
        if (s_pseudonym_table[i].used && s_pseudonym_table[i].query_id == query_id) {
            return &s_pseudonym_table[i];
        }
    }
    return NULL;
}

/**
 * Purge expired pseudonym entries (older than 60 seconds).
 */
static void pseudonym_purge(uint32_t now) {
    const uint32_t PSEUDONYM_EXPIRY_MS = 60000;
    for (int i = 0; i < PSEUDONYM_TABLE_SIZE; i++) {
        if (s_pseudonym_table[i].used && (now - s_pseudonym_table[i].timestamp) > PSEUDONYM_EXPIRY_MS) {
            ESP_LOGD(TAG, "Pseudonym expired: %08" PRIX32, s_pseudonym_table[i].pseudonym);
            s_pseudonym_table[i].used = false;
        }
    }
}

/* ── End pseudonym helpers ─────────────────────────────────────────── */

static void flush_queued_messages(uint32_t dest_addr) {
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (s_queued_msgs[i].used && s_queued_msgs[i].dest_addr == dest_addr) {
            ESP_LOGI(TAG, "Sending queued msg to %08" PRIX32 " (%u bytes)",
                     dest_addr, (unsigned)s_queued_msgs[i].len);
            mesh_send_message(dest_addr, s_queued_msgs[i].data, s_queued_msgs[i].len);
            s_queued_msgs[i].used = false;
        }
    }
}

static void handle_rreq(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RREQ packet");
        return;
    }

    ESP_LOGI(TAG, "RX RREQ query=%08" PRIX32 " dest=%08" PRIX32 " hops=%u metric=%u",
             rreq.query_id, rreq.header.dest_addr, rreq.hop_count, rreq.metric);

    /* RREQ dedup — drop if already seen */
    if (rreq_dedup_check_and_add(&s_rreq_dedup, rreq.query_id, now_ms())) {
        ESP_LOGD(TAG, "Duplicate RREQ query=%08" PRIX32, rreq.query_id);
        return;
    }

    /* Record reverse route (for RREP path back) */
    reverse_route_add(&s_reverse_routes, rreq.query_id, rreq.prev_hop, now_ms());

    /* Is this RREQ for us? */
    if (rreq.header.dest_addr == s_identity->address) {
        ESP_LOGI(TAG, "RREQ is for us — sending RREP");
        bramble_rrep_t rrep = rrep_build_destination(&rreq, s_identity->address);

        /* Route RREP back toward the previous hop */
        reverse_route_t *rev = reverse_route_lookup(&s_reverse_routes, rreq.query_id);
        if (rev) {
            rrep.next_hop = rev->prev_hop;
        }
        send_rrep(&rrep);

        /* Install route to the source via prev_hop */
        uint8_t metric = rreq.metric + compute_link_penalty(rssi, snr);
        route_install(&s_routes, rreq.prev_hop, rreq.prev_hop,
                      rreq.hop_count, metric, ROUTE_ACTIVE, now_ms());
        return;
    }

    /* Not for us — forward the RREQ */
    if (rreq.header.hop_limit > 0) {
        bramble_rreq_t fwd = rreq_forward(&rreq, s_identity->address, rssi, snr);
        send_rreq(&fwd);
    }
}

static void handle_rrep(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RREP packet");
        return;
    }

    ESP_LOGI(TAG, "RX RREP query=%08" PRIX32 " src=%08" PRIX32 " hops=%u",
             rrep.query_id, rrep.src_addr, rrep.hop_count);

    /* Install route to the destination (RREP source) via the sender */
    uint8_t metric = rrep.route_metric + compute_link_penalty(rssi, snr);
    route_install(&s_routes, rrep.src_addr, rrep.header.dest_addr == s_identity->address
                  ? rrep.src_addr : rrep.next_hop,
                  rrep.hop_count, metric, ROUTE_ACTIVE, now_ms());

    /* Is this RREP for us (we originated the RREQ)? */
    pending_discovery_t *pd = discovery_lookup_by_query(&s_pending_disc, rrep.query_id);
    if (pd) {
        ESP_LOGI(TAG, "Route discovered to %08" PRIX32 " (hops=%u, metric=%u)",
                 pd->dest_addr, rrep.hop_count, metric);
        discovery_remove(&s_pending_disc, pd->dest_addr);

        /* Flush queued messages waiting for this route */
        flush_queued_messages(pd->dest_addr);
        return;
    }

    /* Not for us — forward RREP toward the originator via reverse route */
    reverse_route_t *rev = reverse_route_lookup(&s_reverse_routes, rrep.query_id);
    if (rev) {
        bramble_rrep_t fwd = rrep_forward(&rrep, rev->prev_hop);
        send_rrep(&fwd);
    } else {
        ESP_LOGW(TAG, "No reverse route for RREP query=%08" PRIX32, rrep.query_id);
    }
}

static void handle_rerr(const uint8_t *data, uint8_t len) {
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RERR packet");
        return;
    }

    ESP_LOGW(TAG, "RX RERR: dest=%08" PRIX32 " broken_hop=%08" PRIX32,
             rerr.broken_dest, rerr.broken_next_hop);

    /* Invalidate route if it uses the broken next hop */
    route_entry_t *route = route_lookup(&s_routes, rerr.broken_dest);
    if (route && route->next_hop == rerr.broken_next_hop) {
        route->state = ROUTE_BROKEN;
        route->fail_count++;
        ESP_LOGW(TAG, "Route to %08" PRIX32 " marked BROKEN", rerr.broken_dest);

        /* Forward RERR if hop limit allows */
        if (rerr.header.hop_limit > 1) {
            send_rerr(rerr.broken_dest, rerr.broken_next_hop);
        }
    }
}

/* ── Data forwarding for multi-hop ──────────────────────────────────── */

/* ── Mailbox helpers ─────────────────────────────────────────────────── */

static bool mailbox_store(uint32_t dest_addr, const uint8_t *raw, uint8_t raw_len) {
    if (!s_mailbox_enabled) return false;

    /* Find free slot (or oldest) */
    int slot = -1;
    uint32_t oldest_ts = UINT32_MAX;
    int oldest_slot = 0;
    for (int i = 0; i < MAX_MAILBOX_MSGS; i++) {
        if (!s_mailbox[i].used) { slot = i; break; }
        if (s_mailbox[i].timestamp < oldest_ts) {
            oldest_ts = s_mailbox[i].timestamp;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot; /* evict oldest */

    s_mailbox[slot].dest_addr = dest_addr;
    memcpy(s_mailbox[slot].raw_pkt, raw, raw_len);
    s_mailbox[slot].raw_len = raw_len;
    s_mailbox[slot].timestamp = now_ms();
    s_mailbox[slot].used = true;

    ESP_LOGI(TAG, "Mailbox: stored packet for %08" PRIX32 " (slot %d)", dest_addr, slot);
    return true;
}

static void mailbox_flush_for(uint32_t dest_addr) {
    for (int i = 0; i < MAX_MAILBOX_MSGS; i++) {
        if (s_mailbox[i].used && s_mailbox[i].dest_addr == dest_addr) {
            ESP_LOGI(TAG, "Mailbox: delivering stored packet to %08" PRIX32, dest_addr);
            transmit_packet(s_mailbox[i].raw_pkt, s_mailbox[i].raw_len);
            s_mailbox[i].used = false;
        }
    }
}

static void mailbox_expire(uint32_t t) {
    for (int i = 0; i < MAX_MAILBOX_MSGS; i++) {
        if (s_mailbox[i].used && (t - s_mailbox[i].timestamp) > MAILBOX_EXPIRY_MS) {
            ESP_LOGW(TAG, "Mailbox: expired packet for %08" PRIX32, s_mailbox[i].dest_addr);
            s_mailbox[i].used = false;
        }
    }
}

static void forward_data_packet(const uint8_t *data, uint8_t len, const bramble_header_t *header) {
    if (header->hop_limit <= 1) {
        ESP_LOGD(TAG, "Data packet hop limit reached, dropping");
        return;
    }

    /* Look up route to destination */
    route_entry_t *route = route_lookup(&s_routes, header->dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        /* If mailbox enabled, store for later delivery instead of dropping */
        if (s_mailbox_enabled && mailbox_store(header->dest_addr, data, len)) {
            ESP_LOGI(TAG, "No route to %08" PRIX32 " — stored in mailbox", header->dest_addr);
        } else {
            ESP_LOGW(TAG, "No route to forward data for %08" PRIX32, header->dest_addr);
            send_rerr(header->dest_addr, s_identity->address);
        }
        return;
    }

    /* Rebuild header with decremented hop limit */
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    memcpy(buf, data, len);

    bramble_header_t fwd_hdr = *header;
    fwd_hdr.hop_limit--;
    bramble_header_serialize(&fwd_hdr, buf, HEADER_SIZE);

    ESP_LOGI(TAG, "Forwarding data to %08" PRIX32 " via %08" PRIX32,
             header->dest_addr, route->next_hop);
    transmit_packet(buf, len);

    /* Update route usage */
    route->last_used = now_ms();
    route->use_count++;
}

static void mesh_process_rx_packet(const rx_packet_t *pkt) {
    if (pkt->len < HEADER_SIZE) {
        ESP_LOGW(TAG, "Packet too short: %u bytes", pkt->len);
        return;
    }

    bramble_header_t header;
    if (bramble_header_deserialize(&header, pkt->data, pkt->len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid header");
        return;
    }

    /* Record raw RX event */
    traffic_debug_record_rx(&s_traffic_debug, header.type, pkt->len, pkt->rssi);

    /* Dedup check:
     * - include packet type to avoid PROBE vs PROBE_ACK collisions
     * - for PROBE_ACK, include responder addr so multiple peers can respond
     *   to the same probe_id without being collapsed as duplicates.
     */
    uint32_t dedup_key = header.packet_id ^ (((uint32_t)header.type) << 24);
    if (header.type == PKT_TYPE_PROBE_ACK && pkt->len >= HEADER_SIZE + 4) {
        uint32_t probe_ack_resp_addr = 0;
        memcpy(&probe_ack_resp_addr, pkt->data + HEADER_SIZE, 4);
        dedup_key ^= probe_ack_resp_addr;
        if (pkt->len >= HEADER_SIZE + 6) {
            uint8_t probe_round = pkt->data[HEADER_SIZE + 5];
            dedup_key ^= ((uint32_t)probe_round << 16);
        }
    }

    if (dedup_check_and_add(&s_dedup, dedup_key, now_ms())) {
        ESP_LOGD(TAG, "Duplicate packet key=%08" PRIX32 " (pkt=%08" PRIX32 " type=0x%02X)",
                 dedup_key, header.packet_id, header.type);
        /* Note: dedup drop already recorded in initial RX event - no separate event needed */
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.packets_rx++;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "RX type=0x%02X from pkt_id=%08" PRIX32 " RSSI:%d SNR:%d",
             header.type, header.packet_id, pkt->rssi, pkt->snr);

    switch (header.type) {
    case PKT_TYPE_BEACON:
        handle_beacon(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    case PKT_TYPE_ACK:
        handle_ack(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    case PKT_TYPE_DELIVERY_RECEIPT:
        handle_delivery_receipt(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    case PKT_TYPE_RREQ:
        handle_rreq(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    case PKT_TYPE_RREP:
        handle_rrep(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    case PKT_TYPE_RERR:
        handle_rerr(pkt->data, pkt->len);
        break;
    case PKT_TYPE_DATA:
        /* Check if this data is for us or needs forwarding */
        if (header.dest_addr != s_identity->address && header.dest_addr != 0xFFFFFFFF) {
            forward_data_packet(pkt->data, pkt->len, &header);
        } else {
            handle_data(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        }
        break;
    case PKT_TYPE_LOCATION:
        if (header.dest_addr == s_identity->address || header.dest_addr == 0xFFFFFFFF) {
            handle_location(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        }
        break;
    case PKT_TYPE_PROBE:
        handle_probe(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    case PKT_TYPE_PROBE_ACK:
        handle_probe_ack(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled packet type 0x%02X", header.type);
        break;
    }
}

/* ── Adaptive beacon interval controller ────────────────────────────── */

/**
 * Record a churn event (neighbor count change) for adaptive beacon policy.
 */
static void record_churn_event(uint32_t t, uint8_t neighbor_count) {
    s_churn_history[s_churn_history_idx].timestamp = t;
    s_churn_history[s_churn_history_idx].neighbor_count = neighbor_count;
    s_churn_history_idx = (s_churn_history_idx + 1) % MAX_CHURN_HISTORY;
}

/**
 * Calculate churn events in the configured time window.
 */
static uint8_t calculate_churn_events(uint32_t t) {
    uint8_t events = 0;
    uint8_t last_count = 0xff;
    bool first = true;
    
    /* Count neighbor count changes within the window */
    for (int i = 0; i < MAX_CHURN_HISTORY; i++) {
        if (s_churn_history[i].timestamp == 0) continue;
        if ((t - s_churn_history[i].timestamp) > s_beacon_policy.churn_window_ms) continue;
        
        if (first) {
            last_count = s_churn_history[i].neighbor_count;
            first = false;
        } else if (s_churn_history[i].neighbor_count != last_count) {
            events++;
            last_count = s_churn_history[i].neighbor_count;
        }
    }
    return events;
}

/**
 * Compute adaptive beacon interval based on current mesh conditions.
 * Returns new interval in milliseconds.
 */
static uint32_t compute_adaptive_beacon_interval(uint32_t t, uint8_t neighbor_count) {
    if (!s_beacon_policy.enabled || s_beacon_policy.mode != BEACON_MODE_ADAPTIVE) {
        /* Fixed mode: return base interval */
        s_beacon_status.in_backoff = false;
        return s_beacon_policy.base_interval_ms;
    }
    
    /* Calculate churn (neighbor changes) */
    uint8_t churn = calculate_churn_events(t);
    s_beacon_status.churn_events = churn;
    s_beacon_status.neighbor_count = neighbor_count;
    
    beacon_policy_mode_t prev_mode = s_beacon_status.active_mode;
    uint32_t interval = s_beacon_policy.base_interval_ms;
    bool backoff = false;
    
    /* Dense mesh detection: increase interval to reduce airtime */
    if (neighbor_count >= s_beacon_policy.dense_threshold) {
        /* Dense mode: back off to max interval */
        interval = s_beacon_policy.max_interval_ms;
        backoff = true;
        s_beacon_status.active_mode = BEACON_MODE_ADAPTIVE;
    }
    /* Churn detection: temporarily increase beacon rate */
    else if (churn >= s_beacon_policy.churn_threshold) {
        /* Churn mode: increase beacon rate to help convergence */
        interval = s_beacon_policy.min_interval_ms;
        backoff = false;
        s_beacon_status.active_mode = BEACON_MODE_ADAPTIVE;
    }
    /* Stable small mesh: use baseline */
    else {
        interval = s_beacon_policy.base_interval_ms;
        backoff = false;
        s_beacon_status.active_mode = BEACON_MODE_ADAPTIVE;
    }
    
    s_beacon_status.in_backoff = backoff;
    s_beacon_status.current_interval_ms = interval;
    
    /* Log mode transitions */
    if (prev_mode != s_beacon_status.active_mode || 
        (s_beacon_status.last_transition_ms == 0 && s_beacon_policy.enabled)) {
        s_beacon_status.last_transition_ms = t;
        ESP_LOGI(TAG, "Beacon policy: neighbors=%u churn=%u interval=%lums %s",
                 neighbor_count, churn, (unsigned long)interval,
                 backoff ? "DENSE" : (interval < s_beacon_policy.base_interval_ms ? "CHURN" : "STABLE"));
    }
    
    return interval;
}

int mesh_set_beacon_policy(const beacon_policy_config_t *config) {
    if (!config) return -1;
    
    /* Validate config */
    if (config->min_interval_ms < 10000 || config->max_interval_ms > 300000) {
        ESP_LOGE(TAG, "Invalid beacon interval range");
        return -1;
    }
    if (config->min_interval_ms > config->max_interval_ms) {
        ESP_LOGE(TAG, "Min interval must be <= max interval");
        return -1;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_beacon_policy = *config;
    xSemaphoreGive(s_state_mutex);
    
    /* Persist to NVS */
    nvs_handle_t nvs;
    if (nvs_open("bramble_bp", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for beacon policy");
        return -1;
    }
    
    nvs_set_u8(nvs, "enabled", config->enabled ? 1 : 0);
    nvs_set_u8(nvs, "mode", (uint8_t)config->mode);
    nvs_set_u32(nvs, "base_ms", config->base_interval_ms);
    nvs_set_u32(nvs, "min_ms", config->min_interval_ms);
    nvs_set_u32(nvs, "max_ms", config->max_interval_ms);
    nvs_set_u8(nvs, "dense_th", config->dense_threshold);
    nvs_set_u8(nvs, "churn_th", config->churn_threshold);
    nvs_set_u32(nvs, "churn_win", config->churn_window_ms);
    
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist beacon policy to NVS");
        return -1;
    }
    
    ESP_LOGI(TAG, "Beacon policy updated: enabled=%d mode=%d base=%lums",
             config->enabled, config->mode, (unsigned long)config->base_interval_ms);
    return 0;
}

void mesh_get_beacon_policy(beacon_policy_config_t *config) {
    if (!config) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *config = s_beacon_policy;
    xSemaphoreGive(s_state_mutex);
}

void mesh_get_beacon_status(beacon_policy_status_t *status) {
    if (!status) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *status = s_beacon_status;
    xSemaphoreGive(s_state_mutex);
}

void mesh_beacon_policy_load_config(void) {
    nvs_handle_t nvs;
    if (nvs_open("bramble_bp", NVS_READONLY, &nvs) != ESP_OK) {
        /* No saved config, use defaults */
        return;
    }
    
    uint8_t enabled = 0, mode = 0, dense_th = 10, churn_th = 3;
    uint32_t base_ms = 60000, min_ms = 30000, max_ms = 120000, churn_win = 60000;
    
    nvs_get_u8(nvs, "enabled", &enabled);
    nvs_get_u8(nvs, "mode", &mode);
    nvs_get_u32(nvs, "base_ms", &base_ms);
    nvs_get_u32(nvs, "min_ms", &min_ms);
    nvs_get_u32(nvs, "max_ms", &max_ms);
    nvs_get_u8(nvs, "dense_th", &dense_th);
    nvs_get_u8(nvs, "churn_th", &churn_th);
    nvs_get_u32(nvs, "churn_win", &churn_win);
    nvs_close(nvs);
    
    s_beacon_policy.enabled = (enabled != 0);
    s_beacon_policy.mode = (beacon_policy_mode_t)mode;
    s_beacon_policy.base_interval_ms = base_ms;
    s_beacon_policy.min_interval_ms = min_ms;
    s_beacon_policy.max_interval_ms = max_ms;
    s_beacon_policy.dense_threshold = dense_th;
    s_beacon_policy.churn_threshold = churn_th;
    s_beacon_policy.churn_window_ms = churn_win;
    
    ESP_LOGI(TAG, "Loaded beacon policy: enabled=%d mode=%d base=%lums",
             enabled, mode, (unsigned long)base_ms);
}

/* ── Main mesh task ─────────────────────────────────────────────────── */

/**
 * Initialize radio configuration from frequency plan and NVS overrides.
 * Returns ESP_OK on success.
 */
static esp_err_t mesh_init_radio_config(radio_config_t *radio_cfg) {
    ESP_LOGI(TAG, "=== BOOT STAGE: frequency plan init ===");
    const bramble_freq_plan_t *plan = freq_plan_get_default();
    ESP_LOGI(TAG, "Frequency plan: %s (%.1f MHz, max %d dBm)",
             plan->name, plan->default_freq_mhz, plan->max_tx_power_dbm);

    ESP_LOGI(TAG, "=== BOOT STAGE: radio profile config ===");
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, radio_cfg);

    /* Override with frequency plan values */
    radio_cfg->frequency_mhz = plan->default_freq_mhz;
    radio_cfg->tx_power = freq_plan_clamp_power(plan, radio_cfg->tx_power);
    radio_cfg->sf = plan->default_sf;
    radio_cfg->bw_hz = plan->default_bw_hz;

    /* Check NVS for user-saved radio config (overrides defaults) */
    nvs_handle_t nvs;
    if (nvs_open("bramble_radio", NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t freq_khz = 0;
        uint8_t sf = 0, cr = 0;
        uint32_t bw = 0;
        int8_t txp = 0;
        if (nvs_get_u32(nvs, "freq_khz", &freq_khz) == ESP_OK)
            radio_cfg->frequency_mhz = freq_khz / 1000.0f;
        if (nvs_get_u8(nvs, "sf", &sf) == ESP_OK)
            radio_cfg->sf = sf;
        if (nvs_get_u32(nvs, "bw_hz", &bw) == ESP_OK)
            radio_cfg->bw_hz = bw;
        if (nvs_get_i8(nvs, "tx_power", &txp) == ESP_OK)
            radio_cfg->tx_power = freq_plan_clamp_power(plan, txp);
        if (nvs_get_u8(nvs, "cr", &cr) == ESP_OK)
            radio_cfg->coding_rate = cr;
        nvs_close(nvs);
        ESP_LOGI(TAG, "Loaded radio config from NVS");
    }

    ESP_LOGI(TAG, "Radio config: %.1f MHz SF%d BW%lu TX:%d dBm",
             radio_cfg->frequency_mhz, radio_cfg->sf,
             (unsigned long)radio_cfg->bw_hz, radio_cfg->tx_power);

    return ESP_OK;
}

/**
 * Perform periodic maintenance: beacons, neighbor purge, route cleanup, etc.
 */
#define PROBE_SWEEP_ROUNDS 3
#define PROBE_SWEEP_INTERVAL_MS 350
#define PROBE_COLLECTION_WINDOW_MS 5000
#define MAX_PROBE_RESULTS 16

typedef struct {
    uint32_t addr;
    uint8_t  hops;
    int16_t  rssi;
    int8_t   snr;
    uint32_t latency_ms;
    uint8_t  seen_round_mask;
} probe_result_t;

static uint32_t       s_probe_id;
static uint32_t       s_probe_sent_ms;
static bool           s_probe_collecting;
static bool           s_probe_complete_emitted;
static probe_result_t s_probe_results[MAX_PROBE_RESULTS];
static int            s_probe_result_count;
static uint8_t        s_probe_rounds_sent;
static uint32_t       s_probe_next_round_ms;
static bool           s_probe_request_pending;
static uint32_t       s_probe_request_id;

static void mesh_periodic_maintenance(uint32_t t, uint32_t *last_beacon_ms,
                                     uint32_t *beacon_interval,
                                     uint32_t *last_purge_ms) {
    /* Update adaptive beacon interval based on mesh conditions */
    static uint8_t last_neighbor_count = 0;
    uint8_t current_neighbor_count = neighbor_count(&s_neighbors);
    
    /* Record churn event if neighbor count changed */
    if (current_neighbor_count != last_neighbor_count) {
        record_churn_event(t, current_neighbor_count);
        last_neighbor_count = current_neighbor_count;
    }
    
    /* Compute adaptive beacon interval */
    uint32_t base_interval = compute_adaptive_beacon_interval(t, current_neighbor_count);
    
    /* Periodic beacon TX */
    if ((t - *last_beacon_ms) >= *beacon_interval) {
        send_beacon();
        *last_beacon_ms = t;

        /* Add jitter for next interval */
        uint8_t j[2];
        crypto_random(j, 2);
        int32_t jitter = ((int32_t)(j[0] | (j[1] << 8)) % (BEACON_JITTER_MS * 2)) - BEACON_JITTER_MS;
        *beacon_interval = base_interval + jitter;
    }

    /* Periodic neighbor purge + route maintenance */
    if ((t - *last_purge_ms) >= NEIGHBOR_PURGE_INTERVAL) {
        neighbor_purge(&s_neighbors, t);
        dedup_purge(&s_dedup, t);
        route_maintenance(&s_routes, t);
        reverse_route_purge(&s_reverse_routes, t);
        reassembly_purge(&s_reassembly, t);
        pseudonym_purge(t);  /* Clean up expired originator pseudonym mappings */
        *last_purge_ms = t;

        /* Expire old mailbox entries */
        if (s_mailbox_enabled) mailbox_expire(t);

        /* Update shared state */
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.neighbors = s_neighbors;
        xSemaphoreGive(s_state_mutex);

        /* Expire queued messages older than 60s */
        for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
            if (s_queued_msgs[i].used && (t - s_queued_msgs[i].timestamp) > 60000) {
                ESP_LOGW(TAG, "Queued msg for %08" PRIX32 " expired",
                         s_queued_msgs[i].dest_addr);
                s_queued_msgs[i].used = false;
            }
        }
    }

    /* Discovery retries (check every 5s) */
    static uint32_t last_disc_check = 0;
    if ((t - last_disc_check) >= 5000) {
        last_disc_check = t;
        for (int i = 0; i < s_pending_disc.count; i++) {
            pending_discovery_t *pd = &s_pending_disc.entries[i];
            if (discovery_should_retry(pd, t)) {
                if (pd->attempts >= MAX_RREQ_ATTEMPTS) {
                    ESP_LOGW(TAG, "Discovery failed for %08" PRIX32 " after %u attempts",
                             pd->dest_addr, pd->attempts);
                    /* Clear queued messages for this dest */
                    for (int j = 0; j < MAX_QUEUED_MSGS; j++) {
                        if (s_queued_msgs[j].used &&
                            s_queued_msgs[j].dest_addr == pd->dest_addr) {
                            s_queued_msgs[j].used = false;
                        }
                    }
                    discovery_remove(&s_pending_disc, pd->dest_addr);
                    i--; /* re-check same index after remove */
                } else {
                    ESP_LOGI(TAG, "Retrying RREQ for %08" PRIX32 " (attempt %u)",
                             pd->dest_addr, pd->attempts + 1);
                    discovery_record_attempt(pd, t);

                    /* Look up the stored pseudonym for this query_id.
                     * Retries must use the same pseudonym so RREPs can be correlated. */
                    pseudonym_entry_t *ps = pseudonym_lookup_by_query(pd->query_id);
                    uint32_t enc_src;
                    if (ps) {
                        enc_src = ps->pseudonym;
                        ESP_LOGD(TAG, "RREQ retry using stored pseudonym %08" PRIX32, enc_src);
                    } else {
                        /* Pseudonym expired or missing — regenerate (unlikely but safe) */
                        enc_src = pseudonym_generate(s_identity->private_key,
                                                     s_identity->address,
                                                     pd->query_id);
                        pseudonym_store(enc_src, s_identity->address, pd->query_id, t);
                        ESP_LOGW(TAG, "RREQ retry regenerated pseudonym %08" PRIX32, enc_src);
                    }

                    bramble_rreq_t rreq = rreq_build_originator(
                        s_identity->address, pd->dest_addr,
                        pd->query_id, enc_src);
                    send_rreq(&rreq);
                }
            }
        }
    }

    /* ACK retry tick — retransmit unacknowledged packets */
    static uint32_t last_ack_tick = 0;
    if ((t - last_ack_tick) >= 1000) {  /* Check every 1s */
        last_ack_tick = t;
        for (int i = 0; i < MAX_PENDING_ACKS; i++) {
            pending_ack_t *pa = &s_pending_acks.entries[i];
            if (!pa->active) continue;
            if (t >= pa->next_retry_ms) {
                /* Extract packet type from stored packet data for telemetry */
                uint8_t pkt_type = (pa->packet_len >= 2) ? pa->packet_data[1] : 0xFF;
                
                if (pa->attempt >= pa->max_attempts) {
                    ESP_LOGW(TAG, "Message %08" PRIX32 " to %08" PRIX32 " failed after %u attempts",
                             pa->packet_id, pa->dest_addr, pa->attempt);
                    
                    /* Record timeout event - TX fail represents final timeout */
                    traffic_debug_record_tx(&s_traffic_debug, pkt_type, pa->packet_len, pa->tier);
                    
                    msg_store_update_status(pa->packet_id, MSG_STATUS_FAILED);
                    /* Notify webapp of failure */
                    cJSON *params = cJSON_CreateObject();
                    char pkt_buf[12];
                    snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, pa->packet_id);
                    cJSON_AddStringToObject(params, "packet_id", pkt_buf);
                    cJSON_AddStringToObject(params, "status", "failed");
                    rpc_notify("bramble.onAck", params);
                    cJSON_Delete(params);
                    pa->active = false;
                } else {
                    ESP_LOGI(TAG, "Retransmit pkt %08" PRIX32 " to %08" PRIX32 " (attempt %u/%u)",
                             pa->packet_id, pa->dest_addr,
                             pa->attempt + 1, pa->max_attempts);
                    
                    /* Record retry attempt */
                    traffic_debug_record_tx(&s_traffic_debug, pkt_type, pa->packet_len, pa->tier);
                    
                    radio_transmit(pa->packet_data, pa->packet_len);
                    pa->attempt++;
                    pa->next_retry_ms = t + tier_base_delay_ms(pa->tier) * pa->attempt;
                }
            }
        }
    }

    if (s_probe_collecting && s_probe_rounds_sent < PROBE_SWEEP_ROUNDS && t >= s_probe_next_round_ms) {
        uint8_t round = (uint8_t)(s_probe_rounds_sent + 1);
        mesh_send_probe_round(s_probe_id, round);
        s_probe_rounds_sent = round;
        s_probe_next_round_ms = t + PROBE_SWEEP_INTERVAL_MS;
    }

    /* Probe completion event */
    if (s_probe_collecting && !s_probe_complete_emitted && (t - s_probe_sent_ms) >= PROBE_COLLECTION_WINDOW_MS) {
        cJSON *params = cJSON_CreateObject();
        char pid_buf[12];
        snprintf(pid_buf, sizeof(pid_buf), "%08" PRIX32, s_probe_id);
        cJSON_AddStringToObject(params, "probe_id", pid_buf);
        cJSON_AddNumberToObject(params, "unique_count", s_probe_result_count);
        cJSON_AddNumberToObject(params, "duration_ms", t - s_probe_sent_ms);
        cJSON_AddNumberToObject(params, "rounds_total", PROBE_SWEEP_ROUNDS);

        cJSON *responders = cJSON_AddArrayToObject(params, "responders");
        for (int i = 0; i < s_probe_result_count; i++) {
            probe_result_t *r = &s_probe_results[i];
            int seen_rounds = __builtin_popcount((unsigned)r->seen_round_mask);
            cJSON *item = cJSON_CreateObject();
            char addr_buf[12];
            cJSON_AddStringToObject(item, "address", addr_hex(r->addr, addr_buf, sizeof(addr_buf)));
            cJSON_AddNumberToObject(item, "hops", r->hops);
            cJSON_AddNumberToObject(item, "rssi", r->rssi);
            cJSON_AddNumberToObject(item, "snr", r->snr);
            cJSON_AddNumberToObject(item, "latency_ms", r->latency_ms);
            cJSON_AddNumberToObject(item, "seen_rounds", seen_rounds);
            cJSON_AddItemToArray(responders, item);
        }

        rpc_notify("bramble.onProbeComplete", params);
        cJSON_Delete(params);

        s_probe_complete_emitted = true;
        s_probe_collecting = false;
        ESP_LOGI(TAG, "PROBE COMPLETE pid=%08" PRIX32 " unique=%d rounds=%u", s_probe_id,
                 s_probe_result_count, (unsigned)PROBE_SWEEP_ROUNDS);
    }
}

static void mesh_task(void *param) {
    (void)param;

    ESP_LOGI(TAG, "=== BOOT STAGE: mesh_task start (core %d) ===", xPortGetCoreID());

    /* Subscribe this task to the task watchdog timer.
     * If the main loop stalls (or radio_init hangs), the WDT will trigger
     * a reset after CONFIG_ESP_TASK_WDT_TIMEOUT_S seconds. */
    ESP_LOGI(TAG, "=== BOOT STAGE: watchdog subscribe ===");
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not subscribe to task WDT: %d (continuing anyway)", wdt_err);
    }

    /* Initialize radio configuration */
    radio_config_t radio_cfg;
    mesh_init_radio_config(&radio_cfg);

    /* Register radio callbacks before init */
    ESP_LOGI(TAG, "=== BOOT STAGE: register radio callbacks ===");
    radio_set_rx_callback(on_rx);
    radio_set_tx_done_callback(on_tx_done);

    /* Init radio — this is where hangs have been observed on SX1262.
     * The task watchdog will reset the device if radio_init() never returns. */
    ESP_LOGI(TAG, "=== BOOT STAGE: radio_init (SX1262) — WDT active ===");
    int ret = radio_init(&radio_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Radio init failed: %d", ret);
        ESP_LOGE(TAG, "Mesh task exiting — no radio");
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.radio_ok = false;
        xSemaphoreGive(s_state_mutex);
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "=== BOOT STAGE: radio initialized — starting RX ===");
    radio_start_rx();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.radio_ok = true;
    xSemaphoreGive(s_state_mutex);

    /* Timing */
    uint32_t last_beacon_ms = 0;
    uint32_t last_purge_ms = 0;
    uint32_t beacon_interval = BEACON_INTERVAL_MS;

    /* Add initial jitter before first beacon */
    ESP_LOGI(TAG, "=== BOOT STAGE: beacon jitter delay ===");
    uint8_t jitter_buf[2];
    crypto_random(jitter_buf, 2);
    uint32_t initial_delay = (uint32_t)(jitter_buf[0] | (jitter_buf[1] << 8)) % BEACON_JITTER_MS;
    /* Reset WDT during the jitter sleep to avoid spurious WDT triggers */
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(initial_delay));

    /* Fresh WDT reset before send_beacon — TX can block up to 4s waiting for
     * the SX1262 done IRQ.  Without this, jitter_delay + TX can exceed the
     * 5s WDT window and reset the device before the main loop even starts. */
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "=== BOOT STAGE: sending first beacon ===");
    send_beacon();
    last_beacon_ms = now_ms();

    ESP_LOGI(TAG, "=== BOOT STAGE: entering main mesh loop ===");

    /* Main loop */
    while (1) {
        uint32_t t = now_ms();

        /* Reset task watchdog — if this stops being called, WDT resets device */
        esp_task_wdt_reset();

        /* Process received packets */
        rx_packet_t pkt;
        while (xQueueReceive(s_rx_queue, &pkt, 0) == pdTRUE) {
            esp_task_wdt_reset();
            mesh_process_rx_packet(&pkt);
        }

        if (s_mesh_event_queue) {
            mesh_event_type_t mesh_evt;
            while (xQueueReceive(s_mesh_event_queue, &mesh_evt, 0) == pdTRUE) {
                esp_task_wdt_reset();
                if (mesh_evt == MESH_EVT_RECEIPT_TX) {
                    mesh_process_receipt_tx_event();
                }
            }
        }

        /* Start queued probe requests in mesh task context (avoids RPC/SPI contention). */
        uint32_t queued_pid = 0;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_probe_request_pending && !s_probe_collecting) {
            queued_pid = s_probe_request_id;
            s_probe_request_pending = false;
        }
        xSemaphoreGive(s_state_mutex);
        if (queued_pid != 0) {
            mesh_start_probe_sweep(queued_pid);
        }

        mesh_location_policy_tick(t);

        /* Perform all periodic maintenance tasks */
        mesh_periodic_maintenance(t, &last_beacon_ms, &beacon_interval, &last_purge_ms);

        /* Sleep 10ms between polls */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── Send functions ──────────────────────────────────────────────────── */

/**
 * Send a DATA packet. Returns packet_id on success, 0 on failure.
 */
static uint32_t send_data_packet(uint32_t dest_addr, const uint8_t *payload, size_t payload_len,
                                 const uint8_t *nonce, const uint8_t *ciphertext, size_t ct_len,
                                 const uint8_t *tag) {
    /* Build packet: header(12) + src_addr(4) + nonce(12) + ciphertext(N) + tag(16) */
    size_t total = HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + ct_len + BRAMBLE_TAG_SIZE;
    if (total > 255) {
        ESP_LOGE(TAG, "Data packet too large: %u bytes", (unsigned)total);
        return 0;
    }

    uint8_t buf[255];
    uint32_t pkt_id = next_packet_id();
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_DATA,
        .flags = FLAG_ENCRYPT | FLAG_CHANNEL,
        .hop_limit = 3,
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };

    bramble_header_serialize(&header, buf, HEADER_SIZE);
    memcpy(buf + HEADER_SIZE, &s_identity->address, 4);
    memcpy(buf + HEADER_SIZE + 4, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(buf + HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE, ciphertext, ct_len);
    memcpy(buf + HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + ct_len, tag, BRAMBLE_TAG_SIZE);

    int ret = transmit_packet(buf, (uint8_t)total);
    if (ret == 0) {
        /* Estimate airtime: SF9 BW125kHz ≈ 3.7ms/byte + 30ms preamble */
        uint32_t airtime_est = 30 + (uint32_t)(total * 4);
        uint32_t t_now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        airtime_budget_set_mesh_size(&s_airtime, (uint8_t)neighbor_count(&s_neighbors));
        airtime_budget_refill(&s_airtime, t_now);
        airtime_budget_debit(&s_airtime, AIRTIME_TIER_NORMAL, airtime_est);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.packets_tx++;
        s_shared.airtime = s_airtime;
        xSemaphoreGive(s_state_mutex);

        /* Register for ACK tracking (unicast only) */
        if (dest_addr != 0xFFFFFFFF) {
            pending_ack_add(&s_pending_acks, pkt_id, dest_addr,
                            MSG_TIER_NORMAL, buf, (uint16_t)total, now_ms());
        }
        return pkt_id;
    }
    return 0;
}

bool mesh_supports_delivery_event_sync(void) {
    return true;
}

uint32_t mesh_delivery_events_latest_seq(void) {
    uint32_t latest = 0u;
    if (!s_delivery_event_mutex) return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    latest = delivery_event_ring_latest_seq(s_delivery_event_ring);
    xSemaphoreGive(s_delivery_event_mutex);
    return latest;
}

size_t mesh_delivery_events_list_since(uint32_t since_event_seq,
                                       delivery_event_record_t *out,
                                       size_t out_max) {
    size_t count = 0u;
    if (!s_delivery_event_mutex) return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    count = delivery_event_ring_list_since(s_delivery_event_ring, since_event_seq, out, out_max);
    xSemaphoreGive(s_delivery_event_mutex);
    return count;
}

uint32_t mesh_get_last_broadcast_id(void) {
    return s_last_broadcast_id;
}

void mesh_set_broadcast_telemetry_mode(broadcast_telemetry_mode_t mode) {
    if (mode < BROADCAST_TELEMETRY_OFF || mode > BROADCAST_TELEMETRY_PATH_SAMPLED) {
        mode = BROADCAST_TELEMETRY_RECIPIENT_ONLY;
    }
    s_broadcast_telemetry_mode = mode;
}

broadcast_telemetry_mode_t mesh_get_broadcast_telemetry_mode(void) {
    return s_broadcast_telemetry_mode;
}

void mesh_emit_broadcast_delivery_notification(uint32_t src_addr,
                                               uint32_t broadcast_id,
                                               int8_t rssi_at_dest,
                                               uint8_t hop_count,
                                               const uint32_t *relay_path) {
    if (s_broadcast_telemetry_mode == BROADCAST_TELEMETRY_OFF) {
        return;
    }

    char src_buf[12], id_buf[12], hop_buf[12];
    cJSON *params = cJSON_CreateObject();
    snprintf(src_buf, sizeof(src_buf), "%08" PRIX32, src_addr);
    snprintf(id_buf, sizeof(id_buf), "%08" PRIX32, broadcast_id);
    cJSON_AddStringToObject(params, "recipient", src_buf);
    cJSON_AddStringToObject(params, "broadcast_id", id_buf);
    cJSON_AddStringToObject(params, "status", "delivered");
    cJSON_AddNumberToObject(params, "rssi_at_dest", rssi_at_dest);

    if (s_broadcast_telemetry_mode == BROADCAST_TELEMETRY_PATH_SAMPLED && hop_count > 0 && relay_path) {
        uint8_t bounded_hops = (hop_count > DELIVERY_RECEIPT_MAX_HOPS) ? DELIVERY_RECEIPT_MAX_HOPS : hop_count;
        cJSON *path = cJSON_AddArrayToObject(params, "relayPath");
        for (uint8_t i = 0; i < bounded_hops; i++) {
            cJSON *hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, relay_path[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            cJSON_AddItemToArray(path, hop);
        }
    }

    record_broadcast_delivery_event(src_addr, broadcast_id, hop_count, relay_path);

    rpc_notify("bramble.onBroadcastDelivery", params);
    cJSON_Delete(params);
}

int mesh_send_broadcast(const uint8_t *data, size_t len) {
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
        return -1;
    }
    ESP_LOGI(TAG, "mesh_send_broadcast using idx0 channel_id=%u", (unsigned)s_channels[0].channel_id);

    if (!public_channel_can_send(now_ms())) {
        ESP_LOGW(TAG, "Rate limited on public channel");
        return -2;
    }

    /* Check if fragmentation is needed */
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Long message — split into fragments */
        uint16_t msg_id = (uint16_t)(next_packet_id() & 0xFFFF);
        fragment_t *frags = calloc(FRAG_MAX_FRAGMENTS, sizeof(fragment_t));
        if (!frags) {
            ESP_LOGE(TAG, "Fragment allocation failed");
            return -1;
        }

        int num_frags = fragment_split(data, len, msg_id, frags, FRAG_MAX_FRAGMENTS);
        if (num_frags <= 0) {
            free(frags);
            ESP_LOGE(TAG, "Fragment split failed for %u bytes", (unsigned)len);
            return -1;
        }

        uint8_t *ciphertext = malloc(BRAMBLE_MAX_PACKET_SIZE + CHANNEL_MSG_OVERHEAD);
        if (!ciphertext) {
            free(frags);
            ESP_LOGE(TAG, "Ciphertext buffer allocation failed");
            return -1;
        }

        ESP_LOGI(TAG, "Sending broadcast message as %d fragments (msg_id=%04X)", num_frags, msg_id);

        /* Send each fragment with pacing */
        for (int i = 0; i < num_frags; i++) {
            uint8_t nonce[BRAMBLE_NONCE_SIZE];
            uint8_t tag[BRAMBLE_TAG_SIZE];

            int ret = channel_msg_encrypt(&s_channels[0], s_identity->address, 0x01,
                                         frags[i].data, frags[i].len,
                                         nonce, ciphertext, tag);
            if (ret != 0) {
                ESP_LOGE(TAG, "Fragment %d encrypt failed: %d", i, ret);
                continue;
            }

            size_t ct_len = CHANNEL_MSG_OVERHEAD + frags[i].len;
            uint32_t pkt_id = send_data_packet(0xFFFFFFFF, frags[i].data, frags[i].len,
                                              nonce, ciphertext, ct_len, tag);
            if (i == 0 && pkt_id != 0) {
                s_last_broadcast_id = pkt_id;
            }
            if (pkt_id == 0) {
                ESP_LOGW(TAG, "Fragment %d transmission failed", i);
            } else {
                ESP_LOGI(TAG, "Sent fragment %d/%d (pkt_id=%08" PRIX32 ")", i + 1, num_frags, pkt_id);
            }

            /* Inter-fragment pacing to avoid flooding */
            if (i < num_frags - 1) {
                vTaskDelay(pdMS_TO_TICKS(50)); /* 50ms between fragments */
            }
        }

        free(ciphertext);
        free(frags);

        /* Store the full message in message store */
        msg_store_add_ex2(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT,
                          (const char *)data, len, 0, 0,
                          0, MSG_STATUS_NONE, 0);
        return 0;
    }

    /* Short message — fast path (no fragmentation) */
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t ciphertext[BRAMBLE_MAX_PACKET_SIZE + CHANNEL_MSG_OVERHEAD];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    int ret = channel_msg_encrypt(&s_channels[0], s_identity->address, 0x01, /* app_type: text */
                                  data, len, nonce, ciphertext, tag);
    if (ret != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed: %d", ret);
        return ret;
    }

    size_t ct_len = CHANNEL_MSG_OVERHEAD + len;
    uint32_t pkt_id = send_data_packet(0xFFFFFFFF, data, len, nonce, ciphertext, ct_len, tag);
    if (pkt_id != 0) {
        s_last_broadcast_id = pkt_id;
        msg_store_add_ex2(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT,
                          (const char *)data, len, 0, 0,
                          0, MSG_STATUS_NONE, 0);
    }
    return pkt_id ? 0 : -1;
}

uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t *data, size_t len) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGE(TAG, "Invalid channel index: %d (count=%d)", channel_idx, s_num_channels);
        return 0;
    }

    /* Check if fragmentation is needed */
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Long message — split into fragments */
        uint16_t msg_id = (uint16_t)(next_packet_id() & 0xFFFF);
        fragment_t *frags = calloc(FRAG_MAX_FRAGMENTS, sizeof(fragment_t));
        if (!frags) {
            ESP_LOGE(TAG, "Fragment allocation failed");
            return 0;
        }

        int num_frags = fragment_split(data, len, msg_id, frags, FRAG_MAX_FRAGMENTS);
        if (num_frags <= 0) {
            free(frags);
            ESP_LOGE(TAG, "Fragment split failed for %u bytes", (unsigned)len);
            return 0;
        }

        uint8_t *ciphertext = malloc(BRAMBLE_MAX_PACKET_SIZE + CHANNEL_MSG_OVERHEAD);
        if (!ciphertext) {
            free(frags);
            ESP_LOGE(TAG, "Ciphertext buffer allocation failed");
            return 0;
        }

        ESP_LOGI(TAG, "Sending channel message as %d fragments (msg_id=%04X)", num_frags, msg_id);

        uint32_t first_pkt_id = 0;
        /* Send each fragment with pacing */
        for (int i = 0; i < num_frags; i++) {
            uint8_t nonce[BRAMBLE_NONCE_SIZE];
            uint8_t tag[BRAMBLE_TAG_SIZE];

            int ret = channel_msg_encrypt(&s_channels[channel_idx], s_identity->address, 0x01,
                                         frags[i].data, frags[i].len,
                                         nonce, ciphertext, tag);
            if (ret != 0) {
                ESP_LOGE(TAG, "Fragment %d encrypt failed: %d", i, ret);
                continue;
            }

            size_t ct_len = CHANNEL_MSG_OVERHEAD + frags[i].len;
            uint32_t pkt_id = send_data_packet(dest_addr, frags[i].data, frags[i].len,
                                              nonce, ciphertext, ct_len, tag);
            if (pkt_id == 0) {
                ESP_LOGW(TAG, "Fragment %d transmission failed", i);
            } else {
                if (i == 0) first_pkt_id = pkt_id;
                ESP_LOGI(TAG, "Sent fragment %d/%d (pkt_id=%08" PRIX32 ")", i + 1, num_frags, pkt_id);
            }

            /* Inter-fragment pacing to avoid flooding */
            if (i < num_frags - 1) {
                vTaskDelay(pdMS_TO_TICKS(50)); /* 50ms between fragments */
            }
        }

        free(ciphertext);
        free(frags);

        /* Store the full message in message store */
        if (first_pkt_id != 0) {
            msg_store_add_ex2(dest_addr,
                              (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_DIR_BROADCAST_OUT : MSG_DIR_OUTGOING,
                              (const char *)data, len, 0, 0,
                              first_pkt_id,
                              (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_STATUS_NONE : MSG_STATUS_SENT,
                              (int16_t)channel_idx);
        }
        return first_pkt_id;
    }

    /* Short message — fast path (no fragmentation) */
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t ciphertext[BRAMBLE_MAX_PACKET_SIZE + CHANNEL_MSG_OVERHEAD];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    int ret = channel_msg_encrypt(&s_channels[channel_idx], s_identity->address, 0x01,
                                  data, len, nonce, ciphertext, tag);
    if (ret != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed: %d", ret);
        return 0;
    }

    size_t ct_len = CHANNEL_MSG_OVERHEAD + len;
    uint32_t pkt_id = send_data_packet(dest_addr, data, len, nonce, ciphertext, ct_len, tag);
    if (pkt_id != 0) {
        msg_store_add_ex2(dest_addr,
                          (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_DIR_BROADCAST_OUT : MSG_DIR_OUTGOING,
                          (const char *)data, len, 0, 0,
                          pkt_id,
                          (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_STATUS_NONE : MSG_STATUS_SENT,
                          (int16_t)channel_idx);
    }
    return pkt_id;
}

static int queue_message(uint32_t dest_addr, const uint8_t *data, size_t len) {
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used) {
            s_queued_msgs[i].dest_addr = dest_addr;
            memcpy(s_queued_msgs[i].data, data, len);
            s_queued_msgs[i].len = len;
            s_queued_msgs[i].timestamp = now_ms();
            s_queued_msgs[i].used = true;
            ESP_LOGI(TAG, "Queued msg for %08" PRIX32 " (waiting for route)", dest_addr);
            return 0;
        }
    }
    ESP_LOGW(TAG, "Message queue full, dropping msg for %08" PRIX32, dest_addr);
    return -3;
}

static int initiate_discovery(uint32_t dest_addr) {
    if (!rreq_rate_allow(&s_rreq_rl, s_identity->address, dest_addr, now_ms())) {
        ESP_LOGW(TAG, "RREQ rate limited");
        return -1;
    }

    uint32_t query_id = next_packet_id();
    discovery_start(&s_pending_disc, dest_addr, query_id, now_ms());

    /* Generate ephemeral pseudonym for originator privacy.
     * Pseudonym = HMAC-SHA256(private_key, address || query_id)[0..3]
     *
     * This provides unlinkability: each RREQ uses a different pseudonym
     * (query_id acts as nonce), so observers cannot correlate multiple
     * route requests from the same originator.
     *
     * The destination can identify the originator by:
     * 1. Trying HMAC with known peer keys to reverse-map the pseudonym, OR
     * 2. The originator reveals itself during the secure channel setup phase
     *
     * Store mapping locally so incoming RREPs can be correlated. */
    uint32_t pseudonym = pseudonym_generate(s_identity->private_key,
                                             s_identity->address,
                                             query_id);
    pseudonym_store(pseudonym, s_identity->address, query_id, now_ms());

    ESP_LOGI(TAG, "RREQ privacy: addr=%08" PRIX32 " → pseudonym=%08" PRIX32 " (query=%08" PRIX32 ")",
             s_identity->address, pseudonym, query_id);

    uint32_t encrypted_source = pseudonym;
    bramble_rreq_t rreq = rreq_build_originator(s_identity->address, dest_addr,
                                                  query_id, encrypted_source);
    send_rreq(&rreq);
    return 0;
}

uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len) {
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
        return 0;
    }

    /* For non-neighbor destinations, check route table */
    neighbor_entry_t *nb = neighbor_lookup(&s_neighbors, dest_addr);
    if (!nb) {
        /* Not a direct neighbor — need routing */
        route_entry_t *route = route_lookup(&s_routes, dest_addr);
        if (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING) {
            /* No route — start discovery and queue the message */
            if (!discovery_lookup(&s_pending_disc, dest_addr)) {
                initiate_discovery(dest_addr);
            }
            queue_message(dest_addr, data, len);
            /* Still store in msg_store so UI shows it as pending */
            msg_store_add(dest_addr, MSG_DIR_OUTGOING, (const char *)data, len, 0, 0);
            return 1; /* queued — nonzero = success but no packet_id yet */
        }
        /* Have a route — send_data_packet will transmit (next hop gets it) */
    }

    int send_idx = s_default_channel_idx;
    if (send_idx < 0 || send_idx >= s_num_channels) {
        send_idx = 0;
    }
    return mesh_send_channel(send_idx, dest_addr, data, len);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mesh_task_start(bramble_identity_t *identity) {
    s_identity = identity;

    /* Load node name from NVS */
    nvs_handle_t nvs;
    if (nvs_open("bramble", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_node_name);
        esp_err_t name_err = nvs_get_str(nvs, "node_name", s_node_name, &len);
        if (name_err != ESP_OK) {
            if (name_err == ESP_ERR_NVS_INVALID_LENGTH) {
                /* Stored string exceeds buffer — read truncated and force null-terminate */
                len = sizeof(s_node_name);
                nvs_get_str(nvs, "node_name", s_node_name, &len);
                s_node_name[sizeof(s_node_name) - 1] = '\0';
                ESP_LOGW(TAG, "NVS node_name truncated to %u bytes (buffer overflow prevented)",
                         (unsigned)(sizeof(s_node_name) - 1));
            } else {
                s_node_name[0] = '\0';
            }
        } else {
            /* Defensive: ensure null termination even on successful read */
            s_node_name[sizeof(s_node_name) - 1] = '\0';
            if (len > sizeof(s_node_name)) {
                ESP_LOGW(TAG, "NVS node_name length %u exceeds buffer, truncated",
                         (unsigned)len);
            }
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Node name: %s", s_node_name[0] ? s_node_name : "(none)");

    neighbor_init(&s_neighbors);

    /* Derive shared beacon HMAC key from the public channel PSK */
    {
        bramble_channel_t beacon_ch;
        channel_derive_key(BRAMBLE_PUBLIC_CHANNEL_PSK, &beacon_ch);
        memcpy(s_beacon_key, beacon_ch.key, BRAMBLE_KEY_SIZE);
        ESP_LOGI(TAG, "Beacon HMAC key derived from public channel PSK");
    }

    dedup_init(&s_dedup);
    rreq_rate_init(&s_rreq_rl);
    route_init(&s_routes);
    rreq_dedup_init(&s_rreq_dedup);
    reverse_route_init(&s_reverse_routes);
    discovery_init(&s_pending_disc);
    pending_ack_init(&s_pending_acks);
    airtime_budget_init(&s_airtime, (uint32_t)(esp_timer_get_time() / 1000ULL));
    traffic_debug_init(&s_traffic_debug, s_traffic_events, TRAFFIC_DEBUG_CAPACITY);
    mesh_traffic_debug_load_config();  /* Restore persisted debug config */
    traffic_debug_set_notify_callback(&s_traffic_debug, traffic_event_notify, NULL);
    mesh_beacon_policy_load_config();  /* Restore persisted beacon policy config */
    reassembly_init(&s_reassembly);
    location_init(&s_location_mgr);
    memset(s_queued_msgs, 0, sizeof(s_queued_msgs));
    memset(s_pseudonym_table, 0, sizeof(s_pseudonym_table));  /* Init originator pseudonym table */
    s_pseudonym_next_slot = 0;
    memset(&s_shared, 0, sizeof(s_shared));

    /* Initialize public channel (well-known PSK, no key exchange needed) */
    public_channel_init(s_channels, &s_num_channels);
    memset(s_channel_names, 0, sizeof(s_channel_names));
    memset(s_channel_has_psk, 0, sizeof(s_channel_has_psk));
    strncpy(s_channel_names[0], "Broadcast", sizeof(s_channel_names[0]) - 1);
    s_channel_has_psk[0] = false;
    ESP_LOGI(TAG, "Public channel initialized (%d channels)", s_num_channels);

    /* Load additional channels from NVS using channel_storage (Phase 1) */
    {
        bramble_channel_t loaded_channels[MAX_CHANNELS];
        char loaded_names[MAX_CHANNELS][20];
        int loaded_count = 0;
        int loaded_default = 0;

        if (channel_storage_load(loaded_channels, &loaded_count, loaded_names, &loaded_default) == 0 
            && loaded_count > 0) {
            /* Merge loaded channels, preserving channel 0 (public) which is already initialized */
            for (int i = 0; i < loaded_count && s_num_channels < MAX_CHANNELS; i++) {
                /* Skip channel 0 if it was saved (public channel is always first) */
                if (loaded_channels[i].channel_id == 0 && i == 0) {
                    /* Copy the name if it exists */
                    if (loaded_names[i][0] != '\0') {
                        strncpy(s_channel_names[0], loaded_names[i], sizeof(s_channel_names[0]) - 1);
                        s_channel_names[0][sizeof(s_channel_names[0]) - 1] = '\0';
                    }
                    continue;
                }
                
                /* Add to channel list */
                memcpy(&s_channels[s_num_channels], &loaded_channels[i], sizeof(bramble_channel_t));
                
                /* Ensure channel_id matches array index */
                s_channels[s_num_channels].channel_id = (uint8_t)s_num_channels;
                
                /* Copy channel name */
                if (loaded_names[i][0] != '\0') {
                    strncpy(s_channel_names[s_num_channels], loaded_names[i], 
                           sizeof(s_channel_names[s_num_channels]) - 1);
                    s_channel_names[s_num_channels][sizeof(s_channel_names[s_num_channels]) - 1] = '\0';
                }

                /* PSK lock state is loaded separately from NVS metadata. */
                s_channel_has_psk[s_num_channels] = false;
                ESP_LOGI(TAG, "Loaded channel %d from NVS: %s", s_num_channels, 
                        loaded_names[i][0] ? loaded_names[i] : "(unnamed)");
                s_num_channels++;
            }
            
            /* Restore default channel index */
            if (loaded_default >= 0 && loaded_default < s_num_channels) {
                s_default_channel_idx = loaded_default;
            }
            
            mesh_load_channel_psk_flags();
            ESP_LOGI(TAG, "Total channels after NVS load: %d (default=%d)", 
                    s_num_channels, s_default_channel_idx);
        }
    }

    /* Load mailbox enabled state from NVS */
    {
        nvs_handle_t mb_nvs;
        if (nvs_open("bramble_mb", NVS_READONLY, &mb_nvs) == ESP_OK) {
            uint8_t enabled = 0;
            if (nvs_get_u8(mb_nvs, "enabled", &enabled) == ESP_OK) {
                s_mailbox_enabled = (enabled != 0);
                ESP_LOGI(TAG, "Mailbox: %s (from NVS)", s_mailbox_enabled ? "enabled" : "disabled");
            }
            nvs_close(mb_nvs);
        }
        memset(s_mailbox, 0, sizeof(s_mailbox));
    }

    s_state_mutex = xSemaphoreCreateMutex();
    s_delivery_event_mutex = xSemaphoreCreateMutex();
    /* Try PSRAM first (T-Deck Plus), fall back to internal RAM (Heltec V3/V4) */
    s_delivery_event_ring = heap_caps_calloc(1, sizeof(delivery_event_ring_t), MALLOC_CAP_SPIRAM);
    if (!s_delivery_event_ring) {
        ESP_LOGW(TAG, "No PSRAM for delivery ring, using internal RAM (%u bytes)",
                 (unsigned)sizeof(delivery_event_ring_t));
        s_delivery_event_ring = calloc(1, sizeof(delivery_event_ring_t));
    }
    if (!s_delivery_event_ring) {
        ESP_LOGE(TAG, "Failed to allocate delivery event ring (%u bytes)",
                 (unsigned)sizeof(delivery_event_ring_t));
        return;
    }
    delivery_event_ring_init(s_delivery_event_ring);
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_packet_t));
    s_mesh_event_queue = xQueueCreate(MESH_EVENT_QUEUE_DEPTH, sizeof(mesh_event_type_t));
    if (!s_rx_queue || !s_mesh_event_queue) {
        ESP_LOGE(TAG, "Failed to create mesh queues (rx=%p evt=%p)",
                 (void *)s_rx_queue,
                 (void *)s_mesh_event_queue);
        return;
    }

    memset(s_receipt_queue, 0, sizeof(s_receipt_queue));
    s_receipt_timer = NULL;
    esp_timer_create_args_t receipt_timer_args = {
        .callback = mesh_receipt_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "receipt_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t timer_err = esp_timer_create(&receipt_timer_args, &s_receipt_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create receipt timer: %d", (int)timer_err);
        return;
    }

    /* Pin to CPU1 — leave CPU0 for UI/display */
    xTaskCreatePinnedToCore(mesh_task, "mesh", MESH_TASK_STACK, NULL,
                            MESH_TASK_PRIORITY, NULL, 1);
    ESP_LOGI(TAG, "Mesh task created (pinned to CPU1)");
}

void mesh_get_state(mesh_shared_state_t *out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_shared;
    xSemaphoreGive(s_state_mutex);
}

void mesh_get_routes(routing_table_t *out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_routes;
    xSemaphoreGive(s_state_mutex);
}

void mesh_get_location_state(location_manager_t *out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_location_mgr;
    xSemaphoreGive(s_state_mutex);
}

int mesh_add_channel(const char *name, const uint8_t *psk, size_t psk_len) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_num_channels >= MAX_CHANNELS) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    bramble_channel_t *ch = &s_channels[s_num_channels];
    if (psk && psk_len > 0) {
        /* Use provided PSK — treat as passphrase string */
        char psk_str[65];
        size_t copy_len = psk_len < sizeof(psk_str) - 1 ? psk_len : sizeof(psk_str) - 1;
        memcpy(psk_str, psk, copy_len);
        psk_str[copy_len] = '\0';
        channel_derive_key(psk_str, ch);
        s_channel_has_psk[s_num_channels] = true;
    } else {
        /* Generate random key */
        crypto_random(ch->key, BRAMBLE_KEY_SIZE);
        ch->epoch = 0;
        s_channel_has_psk[s_num_channels] = false;
    }
    ch->channel_id = (uint8_t)s_num_channels;

    int idx = s_num_channels;
    s_num_channels++;

    if (name && name[0]) {
        strncpy(s_channel_names[idx], name, sizeof(s_channel_names[idx]) - 1);
        s_channel_names[idx][sizeof(s_channel_names[idx]) - 1] = '\0';
    } else {
        s_channel_names[idx][0] = '\0';
    }

    /* Persist all channels using channel_storage (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) != 0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS");
    }
    mesh_persist_channel_psk_flags();

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Channel added: idx=%d name=%s", idx, name ? name : "(unnamed)");
    return idx;
}

int mesh_remove_channel(int index) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index <= 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1; /* can't remove public channel (0) or invalid index */
    }

    /* Compact array */
    for (int i = index; i < s_num_channels - 1; i++) {
        s_channels[i] = s_channels[i + 1];
        s_channels[i].channel_id = (uint8_t)i;
        strncpy(s_channel_names[i], s_channel_names[i + 1], sizeof(s_channel_names[i]) - 1);
        s_channel_names[i][sizeof(s_channel_names[i]) - 1] = '\0';
        s_channel_has_psk[i] = s_channel_has_psk[i + 1];
    }
    s_channel_names[s_num_channels - 1][0] = '\0';
    s_channel_has_psk[s_num_channels - 1] = false;
    s_num_channels--;

    if (s_default_channel_idx == index) {
        s_default_channel_idx = 0;
    } else if (s_default_channel_idx > index) {
        s_default_channel_idx--;
    }
    if (s_default_channel_idx >= s_num_channels) {
        s_default_channel_idx = 0;
    }

    /* Persist channels after removal (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) != 0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS after removal");
    }
    mesh_persist_channel_psk_flags();

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Channel removed: idx=%d, %d remaining", index, s_num_channels);
    return 0;
}

int mesh_get_channel_count(void) {
    return s_num_channels;
}

const char *mesh_get_channel_name(int index) {
    if (index < 0 || index >= s_num_channels) return NULL;
    if (s_channel_names[index][0]) return s_channel_names[index];

    static char name_buf[20];
    if (index == 0) return "Broadcast";

    nvs_handle_t ch_nvs;
    if (nvs_open("bramble_ch", NVS_READONLY, &ch_nvs) != ESP_OK) {
        return NULL;
    }

    char key_name[20];
    size_t len = sizeof(name_buf);

    /* Current storage key */
    snprintf(key_name, sizeof(key_name), "nm%d", index);
    esp_err_t err = nvs_get_str(ch_nvs, key_name, name_buf, &len);

    /* Backward-compatible legacy key */
    if (err != ESP_OK || name_buf[0] == '\0') {
        len = sizeof(name_buf);
        snprintf(key_name, sizeof(key_name), "ch%d_name", index);
        err = nvs_get_str(ch_nvs, key_name, name_buf, &len);
    }

    nvs_close(ch_nvs);
    if (err != ESP_OK || name_buf[0] == '\0') {
        return NULL;
    }
    strncpy(s_channel_names[index], name_buf, sizeof(s_channel_names[index]) - 1);
    s_channel_names[index][sizeof(s_channel_names[index]) - 1] = '\0';
    return s_channel_names[index];
}

int mesh_get_channel_security(int index, bool *has_psk, uint16_t *epoch) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    if (has_psk) *has_psk = s_channel_has_psk[index];
    if (epoch) *epoch = s_channels[index].epoch;

    xSemaphoreGive(s_state_mutex);
    return 0;
}

void mesh_set_node_name(const char *name) {
    if (name && strlen(name) < sizeof(s_node_name)) {
        strncpy(s_node_name, name, sizeof(s_node_name) - 1);
        s_node_name[sizeof(s_node_name) - 1] = '\0';
    } else {
        s_node_name[0] = '\0';
    }
    ESP_LOGI(TAG, "Node name updated: %s", s_node_name[0] ? s_node_name : "(none)");
}

int mesh_set_node_name_persist(const char *name) {
    if (!name || name[0] == '\0' || strlen(name) >= sizeof(s_node_name)) {
        return -1;
    }

    nvs_handle_t nvs;
    if (nvs_open("bramble", NVS_READWRITE, &nvs) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_str(nvs, "node_name", name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return -1;
    }

    mesh_set_node_name(name);
    return 0;
}

void mesh_set_mailbox(bool enabled) {
    s_mailbox_enabled = enabled;
    ESP_LOGI(TAG, "Mailbox runtime: %s", enabled ? "enabled" : "disabled");
}

bool mesh_get_mailbox(void) {
    return s_mailbox_enabled;
}

/* ── Probe tracking ──────────────────────────────────────────────────── */

static int mesh_send_probe_round(uint32_t pid, uint8_t round) {
    /* Probe packet: header(12) + source_addr(4) + round(1) */
    uint8_t buf[20];
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_PROBE,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0xFFFFFFFF,
        .packet_id = pid,
    };
    bramble_header_serialize(&header, buf, HEADER_SIZE);
    memcpy(buf + HEADER_SIZE, &s_identity->address, 4);
    buf[HEADER_SIZE + 4] = round;

    int rc = transmit_packet(buf, HEADER_SIZE + 5);
    if (rc == 0) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.packets_tx++;
        xSemaphoreGive(s_state_mutex);
    }
    ESP_LOGI(TAG, "PROBE SWEEP TX pid=%08" PRIX32 " round=%u rc=%d", pid, (unsigned)round, rc);
    return rc;
}

static void mesh_start_probe_sweep(uint32_t pid) {
    s_probe_id = pid;
    s_probe_sent_ms = now_ms();
    s_probe_result_count = 0;
    s_probe_collecting = true;
    s_probe_complete_emitted = false;
    s_probe_rounds_sent = 0;
    s_probe_next_round_ms = s_probe_sent_ms;

    /* Round 1 immediate; rounds 2..N sent by periodic maintenance. */
    mesh_send_probe_round(pid, 1);
    s_probe_rounds_sent = 1;
    s_probe_next_round_ms = s_probe_sent_ms + PROBE_SWEEP_INTERVAL_MS;

    ESP_LOGI(TAG, "PROBE SWEEP START pid=%08" PRIX32 " rounds=%u interval_ms=%u", pid,
             (unsigned)PROBE_SWEEP_ROUNDS, (unsigned)PROBE_SWEEP_INTERVAL_MS);
}

uint32_t mesh_send_probe(void) {
    uint32_t pid = next_packet_id();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_probe_request_pending || s_probe_collecting) {
        xSemaphoreGive(s_state_mutex);
        ESP_LOGW(TAG, "PROBE request ignored: busy (pending=%d collecting=%d)",
                 s_probe_request_pending, s_probe_collecting);
        return 0;
    }
    s_probe_request_pending = true;
    s_probe_request_id = pid;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "PROBE SWEEP QUEUED pid=%08" PRIX32, pid);
    return pid;
}

static void handle_probe(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 4) return;

    bramble_header_t header;
    bramble_header_deserialize(&header, data, len);

    uint32_t src_addr;
    memcpy(&src_addr, data + HEADER_SIZE, 4);
    uint8_t probe_round = (len >= HEADER_SIZE + 5) ? data[HEADER_SIZE + 4] : 1;

    char src_buf[12], me_buf[12];
    ESP_LOGI(TAG, "PROBE RX pid=%08" PRIX32 " round=%u src=%s me=%s hop=%u rssi=%d snr=%d",
             header.packet_id,
             (unsigned)probe_round,
             addr_hex(src_addr, src_buf, sizeof(src_buf)),
             addr_hex(s_identity->address, me_buf, sizeof(me_buf)),
             (unsigned)header.hop_limit,
             (int)rssi,
             (int)snr);

    /* Ignore our own probe if it loops back through relays. */
    if (src_addr == s_identity->address) {
        ESP_LOGI(TAG, "PROBE RX ignored self-originated pid=%08" PRIX32, header.packet_id);
        return;
    }

    /* Send probe ACK back */
    uint8_t buf[20];
    bramble_header_t ack_header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_PROBE_ACK,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = src_addr,
        .packet_id = header.packet_id,
    };
    bramble_header_serialize(&ack_header, buf, HEADER_SIZE);
    memcpy(buf + HEADER_SIZE, &s_identity->address, 4);
    buf[HEADER_SIZE + 4] = 1; /* hops = 1 for direct */
    buf[HEADER_SIZE + 5] = probe_round;

    /* Deterministic slotting + bounded jitter for consistent separation among responders. */
    uint32_t slot_ms = 300 + ((s_identity->address % 6) * 110);   /* 300..850 */
    uint32_t jitter_ms = slot_ms + (esp_random() % 120);           /* +0..119 */
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    /* Controlled retries without long tail. */
    for (int i = 0; i < 3; i++) {
        transmit_packet(buf, HEADER_SIZE + 6);
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(140));
    }

    ESP_LOGI(TAG, "PROBE ACK TX pid=%08" PRIX32 " round=%u to=%s from=%s hops=1 jitter=%" PRIu32 "ms x3",
             header.packet_id,
             (unsigned)probe_round,
             addr_hex(src_addr, src_buf, sizeof(src_buf)),
             addr_hex(s_identity->address, me_buf, sizeof(me_buf)),
             jitter_ms);

    /* Forward probe if hop limit allows */
    if (header.hop_limit > 1) {
        bramble_header_t fwd = header;
        fwd.hop_limit--;
        uint8_t fwd_buf[20];
        bramble_header_serialize(&fwd, fwd_buf, HEADER_SIZE);
        memcpy(fwd_buf + HEADER_SIZE, data + HEADER_SIZE, 4);
        transmit_packet(fwd_buf, HEADER_SIZE + 4);
        ESP_LOGI(TAG, "PROBE FWD pid=%08" PRIX32 " new_hop=%u", header.packet_id, (unsigned)fwd.hop_limit);
    }
}

static void handle_probe_ack(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 5) return;

    bramble_header_t header;
    bramble_header_deserialize(&header, data, len);

    char dst_buf[12];

    /* If ACK is not for us, forward it (multi-hop probe result relay). */
    if (header.dest_addr != s_identity->address) {
        if (header.hop_limit > 1) {
            bramble_header_t fwd = header;
            fwd.hop_limit--;
            uint8_t fwd_buf[BRAMBLE_MAX_PACKET_SIZE];
            memcpy(fwd_buf, data, len);
            bramble_header_serialize(&fwd, fwd_buf, HEADER_SIZE);
            transmit_packet(fwd_buf, len);
            ESP_LOGI(TAG, "PROBE ACK FWD pid=%08" PRIX32 " dest=%s hop=%u",
                     header.packet_id,
                     addr_hex(header.dest_addr, dst_buf, sizeof(dst_buf)),
                     (unsigned)fwd.hop_limit);
        } else {
            ESP_LOGI(TAG, "PROBE ACK drop hop-limit pid=%08" PRIX32, header.packet_id);
        }
        return;
    }

    /* Only process if this ACK is for our active probe */
    if (!s_probe_collecting || header.packet_id != s_probe_id) {
        return;
    }

    uint32_t resp_addr;
    memcpy(&resp_addr, data + HEADER_SIZE, 4);
    uint8_t hops = data[HEADER_SIZE + 4];
    uint8_t probe_round = (len >= HEADER_SIZE + 6) ? data[HEADER_SIZE + 5] : 1;
    if (probe_round < 1 || probe_round > PROBE_SWEEP_ROUNDS) probe_round = 1;

    /* Never include self in probe responders. */
    if (resp_addr == s_identity->address) {
        ESP_LOGI(TAG, "PROBE ACK RX ignored self responder pid=%08" PRIX32, header.packet_id);
        return;
    }

    uint32_t latency = now_ms() - s_probe_sent_ms;

    /* Upsert by responder addr: one logical row per responder. */
    int idx = -1;
    for (int i = 0; i < s_probe_result_count; i++) {
        if (s_probe_results[i].addr == resp_addr) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        probe_result_t *r = &s_probe_results[idx];
        r->hops = hops;
        r->latency_ms = latency; /* latest latency */
        if (rssi > r->rssi) r->rssi = rssi; /* best RSSI */
        if (snr > r->snr) r->snr = snr;      /* best SNR */
        r->seen_round_mask |= (uint8_t)(1u << (probe_round - 1));
    } else if (s_probe_result_count < MAX_PROBE_RESULTS) {
        probe_result_t *r = &s_probe_results[s_probe_result_count++];
        r->addr = resp_addr;
        r->hops = hops;
        r->rssi = rssi;
        r->snr = snr;
        r->latency_ms = latency;
        r->seen_round_mask = (uint8_t)(1u << (probe_round - 1));
    }

    char buf[12];
    ESP_LOGI(TAG, "PROBE ACK RX from=%s round=%u hops=%u rssi=%d snr=%d latency=%" PRIu32 "ms pid=%08" PRIX32,
             addr_hex(resp_addr, buf, sizeof(buf)),
             (unsigned)probe_round,
             (unsigned)hops,
             (int)rssi,
             (int)snr,
             now_ms() - s_probe_sent_ms,
             header.packet_id);

    /* Emit notification */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "address", addr_hex(resp_addr, buf, sizeof(buf)));
    cJSON_AddNumberToObject(params, "hops", hops);
    cJSON_AddNumberToObject(params, "rssi", rssi);
    cJSON_AddNumberToObject(params, "snr", snr);
    cJSON_AddNumberToObject(params, "latency_ms", latency);
    cJSON_AddNumberToObject(params, "probe_round", probe_round);
    char pid_buf[12];
    snprintf(pid_buf, sizeof(pid_buf), "%08" PRIX32, s_probe_id);
    cJSON_AddStringToObject(params, "probe_id", pid_buf);
    rpc_notify("bramble.onProbeResult", params);
    cJSON_Delete(params);
}

int mesh_set_default_channel(int index) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    s_default_channel_idx = index;

    /* Persist default channel (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) != 0) {
        ESP_LOGW(TAG, "Failed to persist default channel to NVS");
    }

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Default channel set to idx=%d (broadcast remains public channel 0)", index);
    return 0;
}

const char *mesh_get_node_name(void) {
    if (s_node_name[0] == '\0') return NULL;
    return s_node_name;
}

int mesh_get_identity(uint32_t *addr_out, uint8_t pubkey_out[32]) {
    if (!s_identity || !addr_out || !pubkey_out) return -1;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *addr_out = s_identity->address;
    memcpy(pubkey_out, s_identity->public_key, 32);
    xSemaphoreGive(s_state_mutex);
    return 0;
}

const char *mesh_get_peer_name(uint32_t addr) {
    static char s_name_buf[17];

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    neighbor_entry_t *nb = neighbor_lookup(&s_neighbors, addr);
    if (nb && nb->name[0] != '\0') {
        strncpy(s_name_buf, nb->name, sizeof(s_name_buf) - 1);
        s_name_buf[sizeof(s_name_buf) - 1] = '\0';
        xSemaphoreGive(s_state_mutex);
        return s_name_buf;
    }
    xSemaphoreGive(s_state_mutex);
    return NULL;
}

int mesh_get_channel_info(int *default_idx) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int count = s_num_channels;
    if (default_idx) {
        *default_idx = s_default_channel_idx;
    }
    xSemaphoreGive(s_state_mutex);
    return count;
}

/* ── Traffic debug access ───────────────────────────────────────────── */

static void traffic_event_notify(const traffic_event_t *evt, void *ctx) {
    (void)ctx;
    
    /* Only send notifications if debug is enabled */
    if (!traffic_debug_is_enabled(&s_traffic_debug)) {
        return;
    }
    
    /* Build notification payload */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "seq", evt->seq);
    cJSON_AddNumberToObject(params, "timestamp_ms", evt->timestamp_ms);
    cJSON_AddNumberToObject(params, "pkt_type", evt->pkt_type);
    
    /* Category as string */
    static const char *cat_names[] = {
        "beacon", "timesync", "routing", "ack", "chat", "maintenance", "other"
    };
    if (evt->category < 7) {
        cJSON_AddStringToObject(params, "category", cat_names[evt->category]);
    } else {
        cJSON_AddStringToObject(params, "category", "unknown");
    }
    
    /* Airtime tier as string */
    static const char *tier_names[] = { "none", "normal", "critical", "broadcast" };
    if (evt->airtime_tier <= 3) {
        cJSON_AddStringToObject(params, "airtime_tier", tier_names[evt->airtime_tier]);
    } else {
        cJSON_AddStringToObject(params, "airtime_tier", "unknown");
    }
    
    cJSON_AddNumberToObject(params, "packet_len", evt->packet_len);
    cJSON_AddNumberToObject(params, "rssi", evt->rssi);
    cJSON_AddBoolToObject(params, "is_tx", evt->is_tx);
    
    /* Send notification via RPC notify system (which forwards to WebSocket) */
    rpc_notify("bramble.onTrafficEvent", params);
    
    cJSON_Delete(params);
}

traffic_debug_t *mesh_get_traffic_debug(void) {
    return &s_traffic_debug;
}

void mesh_traffic_debug_set_config(bool enabled, bool include_tx, bool include_rx, uint8_t sample_rate) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    
    /* For now, we only support enabled/disabled.
     * include_tx, include_rx, sample_rate are placeholders for future filtering */
    traffic_debug_enable(&s_traffic_debug, enabled);
    
    /* Persist config to NVS */
    nvs_handle_t nvs;
    if (nvs_open("bramble_tdbg", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "enabled", enabled ? 1 : 0);
        nvs_set_u8(nvs, "inc_tx", include_tx ? 1 : 0);
        nvs_set_u8(nvs, "inc_rx", include_rx ? 1 : 0);
        nvs_set_u8(nvs, "sample", sample_rate);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Traffic debug %s", enabled ? "enabled" : "disabled");
}

void mesh_traffic_debug_get_config(bool *enabled, bool *include_tx, bool *include_rx, uint8_t *sample_rate) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    
    if (enabled) *enabled = traffic_debug_is_enabled(&s_traffic_debug);
    
    /* Load other config from NVS (not yet used in filtering logic) */
    nvs_handle_t nvs;
    if (nvs_open("bramble_tdbg", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t val = 0;
        if (include_tx && nvs_get_u8(nvs, "inc_tx", &val) == ESP_OK)
            *include_tx = (val != 0);
        else if (include_tx)
            *include_tx = true;  /* default */
            
        if (include_rx && nvs_get_u8(nvs, "inc_rx", &val) == ESP_OK)
            *include_rx = (val != 0);
        else if (include_rx)
            *include_rx = true;  /* default */
            
        if (sample_rate && nvs_get_u8(nvs, "sample", &val) == ESP_OK)
            *sample_rate = val;
        else if (sample_rate)
            *sample_rate = 100;  /* default: no sampling */
            
        nvs_close(nvs);
    } else {
        /* NVS read failed, return defaults */
        if (include_tx) *include_tx = true;
        if (include_rx) *include_rx = true;
        if (sample_rate) *sample_rate = 100;
    }
    
    xSemaphoreGive(s_state_mutex);
}

void mesh_traffic_debug_load_config(void) {
    /* Called at startup to restore persisted config */
    nvs_handle_t nvs;
    if (nvs_open("bramble_tdbg", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t enabled = 0;
        if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) {
            traffic_debug_enable(&s_traffic_debug, enabled != 0);
            ESP_LOGI(TAG, "Loaded traffic debug config: enabled=%d", enabled);
        }
        nvs_close(nvs);
    }
}
