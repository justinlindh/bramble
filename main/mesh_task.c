/**
 * Bramble mesh task — runs on CPU1, handles radio TX/RX and protocol dispatch.
 */

#include "mesh_task.h"
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
#include "cJSON.h"

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
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

#define BEACON_INTERVAL_MS      30000   /* 30 seconds between beacons */
#define BEACON_JITTER_MS        5000    /* ±5s random jitter */
#define NEIGHBOR_PURGE_INTERVAL 60000   /* purge expired neighbors every 60s */
#define RX_QUEUE_DEPTH          16
#define MESH_TASK_STACK         8192
#define MESH_TASK_PRIORITY      5

/* ── Received packet queue item ─────────────────────────────────────── */

typedef struct {
    uint8_t data[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t len;
    int16_t rssi;
    int8_t  snr;
} rx_packet_t;

/* ── State ──────────────────────────────────────────────────────────── */

static bramble_identity_t *s_identity;
static neighbor_table_t    s_neighbors;
static dedup_buffer_t      s_dedup;
static rreq_rate_limiter_t s_rreq_rl;
static SemaphoreHandle_t   s_state_mutex;
static QueueHandle_t       s_rx_queue;
static mesh_shared_state_t s_shared;
static char                s_node_name[17] = "";  /* loaded from NVS at startup */

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

/* Reliability — ACK tracking for outgoing unicast messages */
static pending_ack_table_t s_pending_acks;
static airtime_budget_t    s_airtime;

/* Traffic debug telemetry */
#define TRAFFIC_DEBUG_CAPACITY 512
static traffic_event_t     s_traffic_events[TRAFFIC_DEBUG_CAPACITY];
static traffic_debug_t     s_traffic_debug;

/* Channel state */
static bramble_channel_t   s_channels[MAX_CHANNELS];
static char                s_channel_names[MAX_CHANNELS][20];
static int                 s_num_channels = 0;
static int                 s_default_channel_idx = 0; /* unicast default, public broadcast stays channel 0 */

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

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Forward declarations */
static void handle_probe(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr);
static void handle_probe_ack(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr);
static int mesh_send_probe_round(uint32_t pid, uint8_t round);
static void mesh_start_probe_sweep(uint32_t pid);
static void mailbox_flush_for(uint32_t dest_addr);
static int transmit_packet(const uint8_t *buf, uint8_t len);

static const char *addr_hex(uint32_t addr, char *buf, size_t len) {
    snprintf(buf, len, "%08" PRIX32, addr);
    return buf;
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
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

    /* HMAC auth — covers ALL beacon fields (excluding auth_hmac itself) */
    /* Serialize beacon to get canonical byte representation, then HMAC everything
     * up to but not including the auth_hmac field at the end */
    uint8_t hmac_input[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    bramble_beacon_serialize(&beacon, hmac_input, sizeof(hmac_input));
    /* HMAC over header(12) + payload(20) = 32 bytes, excludes auth_hmac[12] at end */
    size_t hmac_len = BEACON_SIZE - sizeof(beacon.auth_hmac);
    uint8_t full_hmac[32];
    crypto_hmac_sha256(s_identity->private_key, 32, hmac_input, hmac_len, full_hmac);
    memcpy(beacon.auth_hmac, full_hmac, sizeof(beacon.auth_hmac));

    uint8_t buf[64];
    if (bramble_beacon_serialize(&beacon, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Beacon serialize failed");
        return -1;
    }

    size_t beacon_wire_len = bramble_beacon_wire_size(&beacon);
    int ret = radio_transmit(buf, (uint8_t)beacon_wire_len);
    if (ret == 0) {
        uint32_t airtime_est = 30 + (uint32_t)(beacon_wire_len * 4);
        uint32_t t_now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        airtime_budget_refill(&s_airtime, t_now);
        airtime_budget_debit(&s_airtime, 0x03, airtime_est);  /* broadcast tier */

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

    /* Sybil detection — check if multiple neighbors cluster at suspiciously similar RSSI */
    {
        int nc = neighbor_count(&s_neighbors);
        if (nc >= 3) {
            int8_t rssi_vals[MAX_NEIGHBORS];
            for (int i = 0; i < nc && i < MAX_NEIGHBORS; i++) {
                rssi_vals[i] = s_neighbors.entries[i].rssi;
            }
            if (sybil_check_rssi_cluster(rssi_vals, nc)) {
                ESP_LOGW(TAG, "SYBIL WARNING: %d neighbors with suspiciously similar RSSI", nc);
            }
        }
    }

    /* Notify any RPC clients that the neighbor table changed */
    rpc_notify("bramble.onNeighborChange", NULL);
}

/* ── ACK handling ────────────────────────────────────────────────────── */

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
    int ret = radio_transmit(buf, (uint8_t)wire_len);
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
    radio_transmit(buf, (uint8_t)wire_len);
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

    /* Update message store status */
    if (msg_store_update_status(ack.ack_packet_id, MSG_STATUS_DELIVERED)) {
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

        /* Relay path from ACK: [dest, relay1, relay2, ...] → prepend sender */
        cJSON *path = cJSON_AddArrayToObject(params, "relayPath");
        char hop_buf[12];
        /* First hop: the sender (us) */
        cJSON *self_hop = cJSON_CreateObject();
        snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, s_identity->address);
        cJSON_AddStringToObject(self_hop, "addr", hop_buf);
        cJSON_AddNumberToObject(self_hop, "rssi", 0);
        cJSON_AddItemToArray(path, self_hop);
        /* Intermediate + destination hops from ACK (reversed: ACK records dest→sender) */
        for (int i = ack.hop_count - 1; i >= 0; i--) {
            cJSON *hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, ack.relay_path[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            /* RSSI: only meaningful for the destination hop */
            cJSON_AddNumberToObject(hop, "rssi", (i == 0) ? ack.rssi_at_dest : 0);
            cJSON_AddItemToArray(path, hop);
        }

        rpc_notify("bramble.onAck", params);
        cJSON_Delete(params);
    }

    if (found) {
        ESP_LOGI(TAG, "Message delivered to %08" PRIX32, ack.src_addr);
    }
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
        /* Null-terminate for printing */
        char text[256];
        size_t tlen = info.data_len;
        if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
        memcpy(text, info.data, tlen);
        text[tlen] = '\0';

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "*** MESSAGE from %08" PRIX32 " ***", info.src_addr);
        ESP_LOGI(TAG, ">>> %s", text);
        ESP_LOGI(TAG, "*** (ch:%d RSSI:%d SNR:%d) ***", info.channel_id, rssi, snr);

        /* Store in message store — check header dest for broadcast detection */
        uint32_t hdr_dest;
        memcpy(&hdr_dest, data + 4, 4);  /* dest_addr at offset 4 in header */
        msg_direction_t dir = (hdr_dest == 0xFFFFFFFF)
            ? MSG_DIR_BROADCAST_IN : MSG_DIR_INCOMING;
        int16_t channel_index = (dir == MSG_DIR_BROADCAST_IN) ? -1 : (int16_t)info.channel_id;
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
            cJSON_AddNumberToObject(params, "channel", (dir == MSG_DIR_BROADCAST_IN) ? -1 : info.channel_id);
            cJSON_AddBoolToObject(params, "broadcast",
                dir == MSG_DIR_BROADCAST_IN);
            rpc_notify("bramble.onMessage", params);
            cJSON_Delete(params);
        }

        /* Send ACK for unicast messages (not broadcasts) */
        if (dir == MSG_DIR_INCOMING) {
            /* Deserialize packet_id from header (big-endian at offset 8) */
            bramble_header_t rx_hdr;
            bramble_header_deserialize(&rx_hdr, data, len);
            send_ack(info.src_addr, rx_hdr.packet_id, rssi);
        }

        /* Also print to stdout for CLI users */
        printf("\n[MSG from %08" PRIX32 "] %s\n", info.src_addr, text);
        printf("bramble> ");
        fflush(stdout);
    }
}

/* ── Routing packet handlers ────────────────────────────────────────── */

static int transmit_packet(const uint8_t *buf, uint8_t len) {
    /* Extract packet type for telemetry (assumes header is already serialized) */
    uint8_t pkt_type = (len >= 2) ? buf[1] : 0xFF;
    
    /* Extract tier from flags (bits 6-7) */
    uint8_t flags = (len >= 3) ? buf[2] : 0;
    uint8_t tier = ((flags >> FLAG_TIER_SHIFT) & 0x03);
    if (tier == 0) tier = 0x01; /* default to normal if not set */
    
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
    /* Periodic beacon TX */
    if ((t - *last_beacon_ms) >= *beacon_interval) {
        send_beacon();
        *last_beacon_ms = t;

        /* Add jitter for next interval */
        uint8_t j[2];
        crypto_random(j, 2);
        int32_t jitter = ((int32_t)(j[0] | (j[1] << 8)) % (BEACON_JITTER_MS * 2)) - BEACON_JITTER_MS;
        *beacon_interval = BEACON_INTERVAL_MS + jitter;
    }

    /* Periodic neighbor purge + route maintenance */
    if ((t - *last_purge_ms) >= NEIGHBOR_PURGE_INTERVAL) {
        neighbor_purge(&s_neighbors, t);
        dedup_purge(&s_dedup, t);
        route_maintenance(&s_routes, t);
        reverse_route_purge(&s_reverse_routes, t);
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
                    uint32_t enc_src = s_identity->address;
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
            mesh_process_rx_packet(&pkt);
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

    int ret = radio_transmit(buf, (uint8_t)total);
    if (ret == 0) {
        /* Estimate airtime: SF9 BW125kHz ≈ 3.7ms/byte + 30ms preamble */
        uint32_t airtime_est = 30 + (uint32_t)(total * 4);
        uint32_t t_now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        airtime_budget_refill(&s_airtime, t_now);
        airtime_budget_debit(&s_airtime, 0x01, airtime_est);  /* normal tier */

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
        msg_store_add_ex2(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT,
                          (const char *)data, len, 0, 0,
                          0, MSG_STATUS_NONE, -1);
    }
    return pkt_id ? 0 : -1;
}

uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t *data, size_t len) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGE(TAG, "Invalid channel index: %d (count=%d)", channel_idx, s_num_channels);
        return 0;
    }

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
                          (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? -1 : (int16_t)channel_idx);
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

    /* Encrypt originator address for privacy — intermediate nodes can't read it.
     * Destination can decrypt by trying XOR with HMAC(shared_key, query_id).
     * We use HMAC(private_key, query_id) as a deterministic mask. The destination,
     * once it receives the RREQ, can try decryption with known peer keys. */
    uint8_t salt_input[4];
    memcpy(salt_input, &query_id, 4);
    uint8_t mask_hash[32];
    crypto_hmac_sha256(s_identity->private_key, 32, salt_input, 4, mask_hash);
    uint32_t mask;
    memcpy(&mask, mask_hash, 4);
    uint32_t encrypted_source = s_identity->address ^ mask;
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
        if (nvs_get_str(nvs, "node_name", s_node_name, &len) != ESP_OK) {
            s_node_name[0] = '\0';
        } else {
            s_node_name[sizeof(s_node_name) - 1] = '\0';  /* Ensure null termination */
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Node name: %s", s_node_name[0] ? s_node_name : "(none)");

    neighbor_init(&s_neighbors);
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
    memset(s_queued_msgs, 0, sizeof(s_queued_msgs));
    memset(&s_shared, 0, sizeof(s_shared));

    /* Initialize public channel (well-known PSK, no key exchange needed) */
    public_channel_init(s_channels, &s_num_channels);
    memset(s_channel_names, 0, sizeof(s_channel_names));
    strncpy(s_channel_names[0], "Broadcast", sizeof(s_channel_names[0]) - 1);
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
                }
                
                ESP_LOGI(TAG, "Loaded channel %d from NVS: %s", s_num_channels, 
                        loaded_names[i][0] ? loaded_names[i] : "(unnamed)");
                s_num_channels++;
            }
            
            /* Restore default channel index */
            if (loaded_default >= 0 && loaded_default < s_num_channels) {
                s_default_channel_idx = loaded_default;
            }
            
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
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_packet_t));

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
    } else {
        /* Generate random key */
        crypto_random(ch->key, BRAMBLE_KEY_SIZE);
        ch->epoch = 0;
    }
    ch->channel_id = (uint8_t)s_num_channels;

    int idx = s_num_channels;
    s_num_channels++;

    if (name && name[0]) {
        strncpy(s_channel_names[idx], name, sizeof(s_channel_names[idx]) - 1);
    } else {
        s_channel_names[idx][0] = '\0';
    }

    /* Persist all channels using channel_storage (Phase 1) */
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) != 0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS");
    }

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
    }
    s_channel_names[s_num_channels - 1][0] = '\0';
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
    snprintf(key_name, sizeof(key_name), "ch%d_name", index);
    size_t len = sizeof(name_buf);
    esp_err_t err = nvs_get_str(ch_nvs, key_name, name_buf, &len);
    nvs_close(ch_nvs);
    if (err != ESP_OK || name_buf[0] == '\0') {
        return NULL;
    }
    strncpy(s_channel_names[index], name_buf, sizeof(s_channel_names[index]) - 1);
    s_channel_names[index][sizeof(s_channel_names[index]) - 1] = '\0';
    return s_channel_names[index];
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

    int rc = radio_transmit(buf, HEADER_SIZE + 5);
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
        radio_transmit(buf, HEADER_SIZE + 6);
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
        radio_transmit(fwd_buf, HEADER_SIZE + 4);
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
            radio_transmit(fwd_buf, len);
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

const char *mesh_get_peer_name(uint32_t addr) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    neighbor_entry_t *nb = neighbor_lookup(&s_neighbors, addr);
    const char *name = (nb && nb->name[0] != '\0') ? nb->name : NULL;
    xSemaphoreGive(s_state_mutex);
    return name;
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
