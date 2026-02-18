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
#include "public_channel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "mesh";

/* ── Configuration ──────────────────────────────────────────────────── */

#define BEACON_INTERVAL_MS      30000   /* 30 seconds between beacons */
#define BEACON_JITTER_MS        5000    /* ±5s random jitter */
#define NEIGHBOR_PURGE_INTERVAL 60000   /* purge expired neighbors every 60s */
#define RX_QUEUE_DEPTH          16
#define MESH_TASK_STACK         8192
#define MESH_TASK_PRIORITY      5

/* ── Received packet queue item ─────────────────────────────────────── */

typedef struct {
    uint8_t data[256];
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

/* Channel state */
static bramble_channel_t   s_channels[MAX_CHANNELS];
static int                 s_num_channels = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */

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
    if (len > sizeof(pkt.data)) len = sizeof(pkt.data);
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
    beacon.battery_pct = 100;  /* TODO: read ADC */
    beacon.tx_queue_depth = 0;
    beacon.neighbor_count = (uint8_t)neighbor_count(&s_neighbors);
    beacon.flags = 0;
    beacon.network_time = 0;  /* TODO: time sync */
    beacon.time_confidence = 0xFFFF;  /* no confidence */

    /* HMAC auth (truncated to 8 bytes) */
    uint8_t hmac_data[20];  /* src_addr(4) + pubkey_hash(4) + uptime(2) + battery(1) + neighbors(1) + ... */
    memcpy(hmac_data, &beacon.src_addr, 4);
    memcpy(hmac_data + 4, &beacon.pubkey_hash, 4);
    memcpy(hmac_data + 8, &beacon.uptime_min, 2);
    hmac_data[10] = beacon.battery_pct;
    hmac_data[11] = beacon.neighbor_count;
    uint8_t full_hmac[32];
    crypto_hmac_sha256(s_identity->private_key, 32, hmac_data, 12, full_hmac);
    memcpy(beacon.auth_hmac, full_hmac, 8);

    uint8_t buf[64];
    if (bramble_beacon_serialize(&beacon, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Beacon serialize failed");
        return -1;
    }

    int ret = radio_transmit(buf, BEACON_SIZE);
    if (ret == 0) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.beacon_tx_count++;
        s_shared.packets_tx++;
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

    /* Check for address collision */
    if (identity_check_collision(s_identity, beacon.src_addr, beacon.pubkey_hash)) {
        ESP_LOGW(TAG, "Address collision detected with %08" PRIX32, beacon.src_addr);
        /* TODO: trigger re-keying */
        return;
    }

    /* Update neighbor table */
    uint32_t t = now_ms();
    int idx = neighbor_update(&s_neighbors, beacon.src_addr, (int8_t)rssi, snr,
                              beacon.pubkey_hash, t);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.beacon_rx_count++;
    s_shared.last_rx_rssi = rssi;
    s_shared.last_rx_snr = snr;
    s_shared.neighbors = s_neighbors;
    xSemaphoreGive(s_state_mutex);

    if (idx >= 0) {
        ESP_LOGI(TAG, "Neighbor %08" PRIX32 " RSSI:%d SNR:%d (total: %d)",
                 beacon.src_addr, rssi, snr, neighbor_count(&s_neighbors));
    }

    /* Notify any RPC clients that the neighbor table changed */
    rpc_notify("bramble.onNeighborChange", NULL);
}

static void handle_data(const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr) {
    (void)rssi; (void)snr;

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
    uint8_t plaintext[256];
    /* We need a buffer for decryption — copy ciphertext for in-place decrypt */
    if (ct_len > sizeof(plaintext)) {
        ESP_LOGW(TAG, "Data too large: %u", (unsigned)ct_len);
        return;
    }
    memcpy(plaintext, ciphertext, ct_len);

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

        /* Also print to stdout for CLI users */
        printf("\n[MSG from %08" PRIX32 "] %s\n", info.src_addr, text);
        printf("bramble> ");
        fflush(stdout);
    }
}

static void handle_rx_packet(const rx_packet_t *pkt) {
    if (pkt->len < HEADER_SIZE) {
        ESP_LOGW(TAG, "Packet too short: %u bytes", pkt->len);
        return;
    }

    bramble_header_t header;
    if (bramble_header_deserialize(&header, pkt->data, pkt->len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid header");
        return;
    }

    /* Dedup check */
    if (dedup_check_and_add(&s_dedup, header.packet_id, now_ms())) {
        ESP_LOGD(TAG, "Duplicate packet %08" PRIX32, header.packet_id);
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
        ESP_LOGD(TAG, "ACK received");
        /* TODO: wire reliability component */
        break;
    case PKT_TYPE_RREQ:
    case PKT_TYPE_RREP:
    case PKT_TYPE_RERR:
        ESP_LOGD(TAG, "Routing packet (type 0x%02X)", header.type);
        /* TODO: wire routing component */
        break;
    case PKT_TYPE_DATA:
        handle_data(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled packet type 0x%02X", header.type);
        break;
    }
}

/* ── Main mesh task ─────────────────────────────────────────────────── */

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

    /* Initialize radio with frequency plan */
    ESP_LOGI(TAG, "=== BOOT STAGE: frequency plan init ===");
    const bramble_freq_plan_t *plan = freq_plan_get_default();
    ESP_LOGI(TAG, "Frequency plan: %s (%.1f MHz, max %d dBm)",
             plan->name, plan->default_freq_mhz, plan->max_tx_power_dbm);

    ESP_LOGI(TAG, "=== BOOT STAGE: radio profile config ===");
    radio_config_t radio_cfg;
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, &radio_cfg);

    /* Override with frequency plan values */
    radio_cfg.frequency_mhz = plan->default_freq_mhz;
    radio_cfg.tx_power = freq_plan_clamp_power(plan, radio_cfg.tx_power);
    radio_cfg.sf = plan->default_sf;
    radio_cfg.bw_hz = plan->default_bw_hz;

    ESP_LOGI(TAG, "Radio config: %.1f MHz SF%d BW%lu TX:%d dBm",
             radio_cfg.frequency_mhz, radio_cfg.sf,
             (unsigned long)radio_cfg.bw_hz, radio_cfg.tx_power);

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
            handle_rx_packet(&pkt);
        }

        /* Periodic beacon TX */
        if ((t - last_beacon_ms) >= beacon_interval) {
            send_beacon();
            last_beacon_ms = t;

            /* Add jitter for next interval */
            uint8_t j[2];
            crypto_random(j, 2);
            int32_t jitter = ((int32_t)(j[0] | (j[1] << 8)) % (BEACON_JITTER_MS * 2)) - BEACON_JITTER_MS;
            beacon_interval = BEACON_INTERVAL_MS + jitter;
        }

        /* Periodic neighbor purge */
        if ((t - last_purge_ms) >= NEIGHBOR_PURGE_INTERVAL) {
            neighbor_purge(&s_neighbors, t);
            dedup_purge(&s_dedup, t);
            last_purge_ms = t;

            /* Update shared state */
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_shared.neighbors = s_neighbors;
            xSemaphoreGive(s_state_mutex);
        }

        /* Sleep 10ms between polls */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── Send functions ──────────────────────────────────────────────────── */

static int send_data_packet(uint32_t dest_addr, const uint8_t *payload, size_t payload_len,
                            const uint8_t *nonce, const uint8_t *ciphertext, size_t ct_len,
                            const uint8_t *tag) {
    /* Build packet: header(12) + src_addr(4) + nonce(12) + ciphertext(N) + tag(16) */
    size_t total = HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + ct_len + BRAMBLE_TAG_SIZE;
    if (total > 255) {
        ESP_LOGE(TAG, "Data packet too large: %u bytes", (unsigned)total);
        return -1;
    }

    uint8_t buf[255];
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_DATA,
        .flags = FLAG_ENCRYPT | FLAG_CHANNEL,
        .hop_limit = 3,
        .dest_addr = dest_addr,
        .packet_id = next_packet_id(),
    };

    bramble_header_serialize(&header, buf, HEADER_SIZE);
    memcpy(buf + HEADER_SIZE, &s_identity->address, 4);
    memcpy(buf + HEADER_SIZE + 4, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(buf + HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE, ciphertext, ct_len);
    memcpy(buf + HEADER_SIZE + 4 + BRAMBLE_NONCE_SIZE + ct_len, tag, BRAMBLE_TAG_SIZE);

    int ret = radio_transmit(buf, (uint8_t)total);
    if (ret == 0) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.packets_tx++;
        xSemaphoreGive(s_state_mutex);
    }
    return ret;
}

int mesh_send_broadcast(const uint8_t *data, size_t len) {
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
        return -1;
    }

    if (!public_channel_can_send(now_ms())) {
        ESP_LOGW(TAG, "Rate limited on public channel");
        return -2;
    }

    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t ciphertext[256 + CHANNEL_MSG_OVERHEAD];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    int ret = channel_msg_encrypt(&s_channels[0], s_identity->address, 0x01, /* app_type: text */
                                  data, len, nonce, ciphertext, tag);
    if (ret != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed: %d", ret);
        return ret;
    }

    size_t ct_len = CHANNEL_MSG_OVERHEAD + len;
    return send_data_packet(0xFFFFFFFF, data, len, nonce, ciphertext, ct_len, tag);
}

int mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len) {
    /* For now, use public channel for all messages (DM encryption needs key exchange) */
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
        return -1;
    }

    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t ciphertext[256 + CHANNEL_MSG_OVERHEAD];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    int ret = channel_msg_encrypt(&s_channels[0], s_identity->address, 0x01,
                                  data, len, nonce, ciphertext, tag);
    if (ret != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed: %d", ret);
        return ret;
    }

    size_t ct_len = CHANNEL_MSG_OVERHEAD + len;
    return send_data_packet(dest_addr, data, len, nonce, ciphertext, ct_len, tag);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mesh_task_start(bramble_identity_t *identity) {
    s_identity = identity;

    neighbor_init(&s_neighbors);
    dedup_init(&s_dedup);
    rreq_rate_init(&s_rreq_rl);
    memset(&s_shared, 0, sizeof(s_shared));

    /* Initialize public channel (well-known PSK, no key exchange needed) */
    public_channel_init(s_channels, &s_num_channels);
    ESP_LOGI(TAG, "Public channel initialized (%d channels)", s_num_channels);

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
