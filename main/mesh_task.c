/**
 * Bramble mesh task: runs on CPU1, handles radio TX/RX and protocol dispatch.
 */

#include "mesh_task.h"
#include "mesh_internal.h"
#include "util.h"
#include "rreq_pseudonym.h"
#include "beacon_policy_calc.h"
#include "probe_results.h"
#include "probe_reply.h"
#include "parked_retry.h"
#include "broadcast_delivery_receipt.h"
#include "rpc_dispatcher.h"
#include "radio.h"
#include "tx_gate.h"
#include "phy_passthrough.h"
#include "packet.h"
#include "crypto.h"
#include "security.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "channel_storage.h"
#include "nonce_counter.h"
#include "replay_window.h"
#include "replay_deferred.h"
#include "dm_session.h"
#include "network_key.h"
#include "routing_auth.h"
#include "identity.h"
#include "identity_store.h"
#include "public_channel.h"
#include "msg_store.h"
#include "discovery.h"
#include "forwarding.h"
#include "channel_flood.h"
#include "reliability.h"
#include "rerr_ack_fastfail.h"
#include "battery.h"
#include "traffic_debug.h"
#include "fragment.h"
#include "location.h"
#include "gps.h"
#include "beacon.h"
#include "timesync.h"
#include "delivery_event_ring.h"
#include "mailbox.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <assert.h>
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
#include "nvs_keys.h"
#include <string.h>
#include <inttypes.h>

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "audio.h"
#endif

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
#include "ui_graphics.h"
#endif

static const char* TAG = "mesh";

/* Forward declarations */
static void traffic_event_notify(const traffic_event_t* evt, void* ctx);

/* ── Configuration ──────────────────────────────────────────────────── */

/* BEACON_JITTER_MS lives in beacon_policy_calc.h next to beacon_next_interval_ms. */
#define BEACON_INTERVAL_MS 60000      /* 60 seconds between beacons (A/B test) */
#define NEIGHBOR_PURGE_INTERVAL 60000 /* purge expired neighbors every 60s */
#define RX_QUEUE_DEPTH 16
#define MESH_EVENT_QUEUE_DEPTH 8
/* handle_data runs on this task and now calls dm_session_ratchet_decrypt, whose
 * dm_recv_walk holds a bounded skip buffer (pending[DM_MAX_SKIP], ~0.58 KB at
 * DM_MAX_SKIP=16) on the stack on top of handle_data's own relay_buf[256] +
 * plaintext[256]. Raised from 8192 to 10240 for comfortable headroom (Task 4;
 * Task 3 stack-headroom concern); the DM_MAX_SKIP 32->16 cut leaves extra margin. */
#define MESH_TASK_STACK 10240
#define MESH_TASK_PRIORITY 5

#define RECEIPT_QUEUE_CAPACITY 8

/* ── State ──────────────────────────────────────────────────────────── */

bramble_identity_t* s_identity;
uint8_t s_beacon_key[BRAMBLE_KEY_SIZE]; /* shared key for beacon HMAC */
neighbor_table_t s_neighbors;
dedup_buffer_t s_dedup;
/* Task 5 (channel flood): a SEPARATE dedup buffer from s_dedup, keyed
 * src_addr-qualified (packet_id ^ src_addr), not just packet_id. s_dedup's
 * key (header.packet_id ^ (type << 24), mesh_process_rx_packet) has no
 * src_addr component; two different originators whose broadcasts happen to
 * collide on packet_id would otherwise have one silently treated as a
 * duplicate of the other's at every shared relay -- harmless for the
 * existing unicast-only traffic s_dedup gates today (a collision there just
 * delays a retry), but a real correctness risk once broadcasts flood
 * multiple hops (the delivery-path audit's flagged concern). Kept as its
 * own instance rather than widening s_dedup's key so this stays scoped to
 * the flood path and cannot change dedup behavior for RREQ/PROBE_ACK/other
 * control traffic that already relies on s_dedup's existing key shape. */
dedup_buffer_t s_flood_dedup;
/* Task 6 (GAP A): tracks unicast DATA we have already delivered locally
 * (ACK already sent), keyed the same src_addr-qualified way s_flood_dedup
 * is (packet_id ^ src_addr, collision-safe for the same reason). Consulted
 * ONLY at mesh_process_rx_packet's s_dedup duplicate-hit branch: without
 * this, a duplicate unicast DATA (the sender's own retransmit after its
 * first ACK was lost) is silently dropped there and NEVER re-ACKed, making
 * a single lost ACK terminal (the message was delivered, but the sender
 * eventually marks it FAILED). Recorded once, right after send_ack, for
 * every genuinely new unicast delivery in handle_data; a later duplicate is
 * recognized via dedup_contains (a pure peek, never inserts on a miss) and
 * re-triggers send_ack WITHOUT re-entering handle_data's decrypt/deliver
 * path, so local delivery stays exactly-once. */
static dedup_buffer_t s_delivered_dedup;
replay_table_t s_replay;         /* SEC-M1: per-sender authenticated nonce-counter replay window */
replay_table_t s_control_replay; /* ws 1.3b: control-plane (RREP/RERR/ACK/receipt/beacon)
                                           replay window, keyed on the authenticated signer address,
                                           separate from the data-plane s_replay above */
/* Per-node identity Phase 3 (Part C): this node's verified TOFU pin table
 * (address -> Ed25519/X25519 pubs), fed by handle_identity_attestation
 * below. Persisted to NVS (DM forward-secrecy + SAS): new pins and verified
 * bits survive reboot via mesh_pin_store_save/_load. */
identity_store_t s_identity_pins;
/* Pin-store NVS persistence (defined near the other NVS helpers). save() is
 * called whenever a NEW pin is added or a verified bit changes; load() runs
 * once at boot after the anchor is provisioned. */
static replay_deferred_t s_deferred; /* tier-2: deferred acceptance for delayed CHAT (Task 0.6) */
/* RREQ origination gate. Forwarded RREQs are gated separately, by the global
 * s_rreq_fwd_rl budget below (ws 1.3d, SEC-M4); see SECURITY-MODEL.md for the
 * node-global-not-per-neighbor residual. */
static rreq_rate_limiter_t s_rreq_rl;
/* Global forwarded-RREQ token bucket (ws 1.3d, SEC-M4). Bounds this node's
 * aggregate forwarded-RREQ rate regardless of the unauthenticated, spoofable
 * rreq.prev_hop field; not keyed per-neighbor on purpose (see
 * SECURITY-MODEL.md). */
rreq_fwd_limiter_t s_rreq_fwd_rl;
SemaphoreHandle_t s_state_mutex;
SemaphoreHandle_t s_delivery_event_mutex;
/* Guards nonce_counter_next only: send_data_packet is reachable from the
 * mesh task, the RPC/httpd task, the LVGL UI task, and the CLI task, all
 * without a lock of their own, and the nonce counter itself has no internal
 * locking (kept host-testable, no FreeRTOS dependency). A mutex, not a
 * critical section, because a boundary flush may block on NVS I/O. */
SemaphoreHandle_t s_nonce_mutex;
/*
 * DM session table (SEC-C2, Task 1.4). Guards every dm_lookup/dm_alloc,
 * every session state transition, and every read of the ratchet state for
 * dm_session_ratchet_encrypt/decrypt. Reachable from the mesh RX task
 * (handle_ke_envelope, handle_data's session-decrypt path),
 * handshake_worker_task (the DH compute + state update), and the TX-side
 * callers of mesh_send_message/mesh_send_channel (RPC/httpd, LVGL UI, CLI
 * tasks), same multi-task shape as s_nonce_mutex.
 *
 * Locking discipline: s_dm_mutex is always the OUTER lock relative to
 * s_nonce_mutex. Anything that needs both (send_dm_packet, called with
 * s_dm_mutex already held, takes s_nonce_mutex internally exactly like
 * send_data_packet does) takes s_dm_mutex first and releases s_nonce_mutex
 * before releasing s_dm_mutex. Nothing ever takes s_nonce_mutex first and
 * then reaches for s_dm_mutex, so the two can never deadlock on each other.
 */
SemaphoreHandle_t s_dm_mutex;
dm_table_t* s_dm_table;
static QueueHandle_t s_rx_queue;
QueueHandle_t s_mesh_event_queue;
mesh_shared_state_t s_shared;

pending_receipt_t s_receipt_queue[RECEIPT_QUEUE_CAPACITY];
esp_timer_handle_t s_receipt_timer;

pending_probe_reply_t s_probe_reply_queue[PROBE_REPLY_QUEUE_CAPACITY];
esp_timer_handle_t s_probe_reply_timer;

delivery_event_ring_t* s_delivery_event_ring;
char s_node_name[BRAMBLE_NODE_NAME_MAX + 1] = ""; /* loaded from NVS at startup */

/* Routing state */
routing_table_t s_routes;
rreq_dedup_t s_rreq_dedup;
reverse_route_table_t s_reverse_routes;
pending_discovery_table_t s_pending_disc;

/* Queued messages waiting for route discovery (QUEUE_REASON_ROUTE) or for a
 * DM session to establish (QUEUE_REASON_SESSION, Task 1.4 / SEC-C2 B5). Both
 * reasons share this one array; `reason` disambiguates so the reaper, the
 * route-established flush, and the session-established flush each only
 * touch entries meant for them. */
#define MAX_QUEUED_MSGS 8
#define QUEUE_REASON_ROUTE 0
#define QUEUE_REASON_SESSION 1
/* B5: covers the full handshake retransmit budget (base 4s, x2 backoff,
 * cap 64s, 5 attempts, worst case ~124s) plus margin, so a slow-SF path
 * cannot have its awaiting-session message reaped before the handshake can
 * possibly complete. Route-awaiting entries keep the pre-existing flat
 * 60000ms below. */
#define DM_QUEUE_TTL_MS 150000
queued_msg_t s_queued_msgs[MAX_QUEUED_MSGS];

/*
 * Handshake dedup (SEC-C2 item 5): a replayed INIT (same src_addr, same
 * ephemeral pubkey, same ke_epoch) must not re-run the quad-DH state
 * machine or re-emit a RESP, or an attacker can loop expensive handshake
 * work and RESP retransmission for free. Checked (and recorded) in
 * handle_ke_envelope BEFORE the work item is posted to the worker, so a
 * replay never even reaches the M7 offload path, let alone the DH itself.
 * Small fixed ring, LRU by seen_ms; not a security boundary in itself (it
 * only suppresses redundant work), so no cap-based DoS concern the way
 * DM_MAX_HANDSHAKING is.
 */
#define DM_HS_DEDUP_MAX 16
dm_hs_dedup_entry_t s_hs_dedup[DM_HS_DEDUP_MAX];

/*
 * Pending-initiator ephemeral keys (Task 1.4). dm_session_t (Task 1.2,
 * already locked in and unit-tested) has no field for "my own in-flight
 * handshake ephemeral": it stores only the FINAL session key. To verify a
 * RESP via dm_verify_resp, the initiator needs the ephemeral keypair it
 * generated when it sent the matching INIT, so mesh_task.c (the
 * integration layer) owns this small side table instead of growing
 * dm_session_t. Sized to DM_MAX_HANDSHAKING since only handshaking peers
 * need one.
 */
dm_pending_eph_t s_pending_eph[DM_MAX_HANDSHAKING];

/* M7 offload (RFC compute-placement): the four X25519 scalar multiplications
 * a handshake message requires must not run inline on the mesh RX loop.
 * handle_ke_envelope does only cheap parsing/validation and posts a work
 * item here; handshake_worker_task drains it on a low-priority task. */
#define HANDSHAKE_WORK_QUEUE_LEN 8
/* The DM handshake is a quad-DH X25519 exchange plus HKDF and an Ed25519
 * identity verify, not a single "periodic X25519": at 4096 bytes the
 * dm_hs_worker task stack-overflowed and rebooted the receiver on the first
 * incoming key exchange (found in 2-node on-device testing 2026-07-06;
 * host/gosim have no real FreeRTOS task stacks so never hit it). */
#define DM_HANDSHAKE_WORKER_STACK 8192
#define DM_HANDSHAKE_WORKER_PRIORITY (MESH_TASK_PRIORITY - 2)
QueueHandle_t s_handshake_work_q;

/* Jittered channel-flood relay queue (Task 5). Same shape and drain cadence
 * as the RREQ forward queue below, holding the exact relay-mutated wire
 * bytes (hop_limit decremented, prev_hop rewritten to us) a broadcast DATA
 * frame is rebroadcast with once its jitter elapses. pending_flood_relay_t,
 * FLOOD_RELAY_QUEUE_CAPACITY and the rebroadcast-suppression helper live in
 * channel_flood.h (Flooding F1) so they are unit-testable in isolation. */
pending_flood_relay_t s_flood_relay_queue[FLOOD_RELAY_QUEUE_CAPACITY];

/* Flood relays dropped because the queue above was full (issue #87). A drop
 * under congestion that nobody can see is indistinguishable in the field
 * from a radio problem, so it is counted and surfaced through
 * bramble.getDiagnostics. */
uint32_t s_flood_relay_drops;

/* PROBE ingress backpressure (issue #75). PROBE stays unauthenticated by
 * design; these buckets only bound how much transmission an inbound probe
 * can buy. Global, not per-sender, for the same reason SEC-M4's forwarded-
 * RREQ cap is: see security.h. */
probe_ingress_limiter_t s_probe_ingress;

/* Jittered RREQ forward queue (DES-3). Relays delay RREQ rebroadcasts by a
 * random 50-300ms so same-hop relays do not key up at the same instant; the
 * mesh task drains due entries from its main loop (10ms poll cadence). */
#define RREQ_FWD_QUEUE_CAPACITY 8
pending_rreq_fwd_t s_rreq_fwd_queue[RREQ_FWD_QUEUE_CAPACITY];

/* Reliability: ACK tracking for outgoing unicast messages */
pending_ack_table_t s_pending_acks;

/* Traffic debug telemetry. Depth comes from TRAFFIC_DEBUG_CAPACITY in
 * traffic_debug.h, which each platform's build may override. */
static traffic_event_t s_traffic_events[TRAFFIC_DEBUG_CAPACITY];
static traffic_debug_t s_traffic_debug;
timesync_state_t s_timesync;

/* Identity attestation TX cadence state (SHARED: armed/reset in mesh_beacon.c,
 * timing-gated in mesh_periodic_maintenance here). Relocated to the state
 * region so the attestation TX code can move to mesh_beacon.c cleanly. */
uint32_t s_attestation_last_ms;
uint32_t s_attestation_wait_ms; /* 0 = boot send not attempted yet */

/* Fragment reassembly context */
static reassembly_ctx_t s_reassembly;

/* Adaptive beacon interval policy */
beacon_policy_config_t s_beacon_policy = {
    .enabled = false, /* Default: disabled (fixed 60s) */
    .mode = BEACON_MODE_FIXED,
    .base_interval_ms = 60000,
    .min_interval_ms = 30000,
    .max_interval_ms = 120000,
    .dense_threshold = 10,
    .churn_threshold = 3,
    .churn_window_ms = 60000,
};
beacon_policy_status_t s_beacon_status = {
    .active_mode = BEACON_MODE_FIXED,
    .current_interval_ms = 60000,
    .neighbor_count = 0,
    .churn_events = 0,
    .last_transition_ms = 0,
    .in_backoff = false,
};
churn_sample_t s_churn_history[MAX_CHURN_HISTORY];
int s_churn_history_idx = 0;

/* Channel state */
bramble_channel_t s_channels[MAX_CHANNELS];
char s_channel_names[MAX_CHANNELS][20];
bool s_channel_has_psk[MAX_CHANNELS];
int s_num_channels = 0;
int s_default_channel_idx = 0; /* unicast default, public broadcast stays channel 0 */
uint32_t s_last_broadcast_id = 0;
uint32_t s_recent_broadcast_ids[RECENT_BROADCAST_RING_SIZE];
int s_recent_broadcast_idx = 0;
broadcast_telemetry_mode_t s_broadcast_telemetry_mode = BROADCAST_TELEMETRY_RECIPIENT_ONLY;

/* Mailbox: store-and-forward for offline neighbors (backed by components/mailbox) */
bool s_mailbox_enabled = false;
mailbox_t s_mailbox;

/* Flooding F1 Task 1: runtime toggle for the unicast flood transport. OFF
 * (default) preserves today's reactive route-lookup forward for unicast
 * DATA; ON routes unicast DATA not addressed to us through the same
 * multi-hop flood engine broadcast DATA already uses (channel_flood_decide +
 * s_flood_dedup) instead of forward_data_packet. See mesh_process_rx_packet's
 * PKT_TYPE_DATA case. NVS-persisted, same pattern as s_mailbox_enabled. */
bool s_flood_transport = false;

/* Flooding F1 finalize: operator-settable flood-transport origination hop
 * budget. Under s_flood_transport, a freshly-originated flood DATA and its
 * flooded-ACK are stamped with this hop_limit (via flood_origination_hop_limit)
 * instead of the hardcoded ROUTE_HOP_LIMIT_MAX, so reach can be matched to the
 * expected network diameter. Default FLOOD_HOP_LIMIT_DEFAULT (8) leaves shipped
 * behavior unchanged; NVS-persisted (NVS_NS_FLOOD "hop_limit"); clamped to
 * [FLOOD_HOP_LIMIT_MIN, FLOOD_HOP_LIMIT_CEIL]. SEPARATE from ROUTE_HOP_LIMIT_MAX
 * (the reactive path is untouched). */
uint8_t s_flood_hop_limit = FLOOD_HOP_LIMIT_DEFAULT;

/* Location policy engine tick state */
uint32_t s_location_last_policy_tick_ms = 0;
uint32_t s_location_last_send_ms = 0;
location_manager_t s_location_mgr;

/* ── Helpers ────────────────────────────────────────────────────────── */

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

/* Copy the live neighbor table into the mutex-guarded snapshot the UI and RPC
 * read. Anything that mutates s_neighbors outside the periodic maintenance
 * tick publishes it, or the change stays invisible for up to a purge interval.
 * handle_beacon is the one mutator that does NOT call this: it publishes
 * inline, batched into the same critical section that bumps beacon_rx_count
 * and last_rx_rssi/snr, and routing it through here would take a
 * non-recursive mutex twice. */
static void mesh_publish_neighbors(void) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.neighbors = s_neighbors;
    xSemaphoreGive(s_state_mutex);
}

void mesh_note_peer_heard(uint32_t addr, int16_t rssi, int8_t snr) {
    if (addr == 0 || !s_identity || addr == s_identity->address)
        return;
    /* Only refreshes an address a beacon already admitted (neighbor_touch
     * never creates entries), and only ever called with an address the frame's
     * own MAC covers, so this widens liveness without widening trust. */
    if (neighbor_touch(&s_neighbors, addr, (int8_t)rssi, snr, now_ms()))
        mesh_publish_neighbors();
}

uint32_t next_packet_id(void) {
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

/* Issue #72: ask the mesh task to flush the replay high-water marks before a
 * deliberate reboot (OTA, RPC reboot, settings screen). Deliberately a FLAG
 * rather than a direct write: the mesh task is the only mutator of the
 * replay tables, so serializing them from the timer/RPC task could snapshot
 * a torn state. The mesh loop polls every 10 ms and every reboot path grants
 * at least 500 ms, so the flush lands well before the restart. A crash or a
 * bare esp_restart still falls back to the periodic flush bound. */
static volatile bool s_replay_flush_requested;

void mesh_reboot_delayed(int delay_ms) {
    s_replay_flush_requested = true;
    if (delay_ms <= 0)
        delay_ms = 100;
    TimerHandle_t t =
        xTimerCreate("reboot", pdMS_TO_TICKS(delay_ms), pdFALSE, NULL, reboot_timer_cb);
    if (t == NULL) {
        ESP_LOGE(TAG, "Failed to create reboot timer, rebooting immediately");
        esp_restart();
        return;
    }
    if (xTimerStart(t, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start reboot timer, rebooting immediately");
        esp_restart();
    } else {
        ESP_LOGI(TAG, "Reboot scheduled in %d ms", delay_ms);
    }
}

/* ── Radio callbacks (ISR context → queue) ──────────────────────────── */

static void on_rx(const uint8_t* data, uint8_t len, const radio_rx_info_t* info) {
    /* PHY passthrough pre-hook (DESIGN.md section 10): when the hardware-bridge
     * mode is active, forward the raw frame up the RPC/serial link with its
     * radio metadata BEFORE it is handed to the mesh. This is a tap, not a
     * diversion: normal mesh processing still runs below. The intended gateway
     * (a bare, unprovisioned Heltec) is inert to the mesh anyway, so the extra
     * queue push is harmless; keeping the path additive avoids perturbing the
     * firmware's own receive handling for anyone who force-enables on a live
     * node. The carrier is read from the live radio config only while active,
     * so the common (inactive) case adds a single flag check. */
    if (phy_passthrough_is_active()) {
        radio_config_t rcfg;
        radio_get_config(&rcfg);
        uint32_t freq_hz = (uint32_t)(rcfg.frequency_mhz * 1000000.0f);
        phy_passthrough_forward_rx(data, len, info, freq_hz);
    }

    rx_packet_t pkt;
    /* len is uint8_t (max 255), pkt.data is 256 bytes; always fits */
    memcpy(pkt.data, data, len);
    pkt.len = len;
    pkt.rssi = info->rssi;
    pkt.snr = info->snr;

    /* Called from radio task context, not ISR */
    if (xQueueSend(s_rx_queue, &pkt, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, dropping packet");
    }
}

static void on_tx_done(void) { ESP_LOGD(TAG, "TX complete"); }

/* Confirm a data message that was just handed to the message store: ACK a
 * unicast and record the delivery (Task 6 / GAP A) so the sender's retransmit
 * after a lost ACK is re-ACKed at mesh_process_rx_packet's dedup gate rather
 * than silently dropped, keyed like s_flood_dedup (packet_id ^ src_addr); or,
 * for a broadcast, queue a delivery receipt when policy calls for one. The
 * single-packet and fragment-reassembly delivery paths share this and differ
 * only in the packet_id used for the broadcast receipt: a reassembled message
 * reports its first-received fragment's id.
 *
 * peer_addr and wire_src_addr are deliberately SEPARATE parameters and must
 * not be collapsed into one. peer_addr is the sender as the delivered payload
 * reports it (channel_msg_info_t.src_addr), which is what the ACK and the
 * delivery receipt are addressed to. wire_src_addr is the src_addr read off
 * the still-plaintext wire prefix at BRAMBLE_DATA_SRC_ADDR_OFFSET, and it is
 * the ONLY correct input to the s_delivered_dedup key: the consumer of that
 * key (mesh_process_rx_packet's duplicate-DATA re-ACK branch) recomputes it
 * from the wire the same way, because a duplicate is never decrypted there.
 * The two are equal for a DM (handle_data assigns info.src_addr = src_addr)
 * but NOT structurally equal for a channel message, whose info.src_addr comes
 * out of the decrypted inner plaintext (channel_msg.c) while the AEAD AAD
 * binds only the wire value (SEC-M2). Keying the record on the payload value
 * would leave a mismatched entry the re-ACK gate can never hit, quietly
 * restoring the lost-ACK-is-terminal failure GAP A exists to prevent. */
static void confirm_data_delivery(msg_direction_t dir, uint32_t peer_addr, uint32_t wire_src_addr,
                                  uint32_t dest_addr, uint32_t ack_packet_id,
                                  uint32_t receipt_packet_id, int16_t rssi) {
    if (dir == MSG_DIR_INCOMING) {
        send_ack(peer_addr, ack_packet_id, rssi);
        dedup_check_and_add(&s_delivered_dedup, ack_packet_id ^ wire_src_addr, now_ms());
    } else if (mesh_should_emit_broadcast_delivery_receipt(dest_addr,
                                                           (uint8_t)neighbor_count(&s_neighbors))) {
        queue_broadcast_delivery_receipt(peer_addr, receipt_packet_id);
    }
}

static void handle_data(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    /* Data packet layout (wire v4): header(12) + src_addr(4) + prev_hop(4) +
     * nonce(12) + ciphertext(N) + tag(16). prev_hop is relay-mutable/
     * MAC-excluded (packet.h); reverse-route learning off it happens once,
     * at the mesh_process_rx_packet dispatch site, not here. */
    if (len < BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE + 1) {
        ESP_LOGW(TAG, "Data packet too short: %u", len);
        return;
    }

    uint32_t src_addr;
    memcpy(&src_addr, data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);

    /* Self-originated guard (mirrors handle_location): the Task 5 channel
     * flood means a node now hears its OWN broadcast/channel DATA echoed
     * back by a neighbor's rebroadcast. Without this, the frame below would
     * still get trial-decrypted (this node holds the key it encrypted with)
     * and re-delivered as a spurious incoming onMessage/msg_store_add, plus
     * a self-ACK and an s_delivered_dedup record for channel messages. The
     * flood relay decision a few lines down already independently forces
     * should_relay=false for a self-echo via is_own_echo, so returning here
     * before that logic runs does not change relay behavior for this frame;
     * it only skips the decrypt/deliver path that follows. Frames from
     * other sources are completely unaffected by this check. */
    if (src_addr == s_identity->address) {
        return;
    }

    const uint8_t* nonce = data + BRAMBLE_DATA_NONCE_OFFSET;
    size_t ct_len = len - BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;
    const uint8_t* ciphertext = nonce + BRAMBLE_NONCE_SIZE;
    const uint8_t* tag = ciphertext + ct_len;

    channel_msg_info_t info;
    if (ct_len > BRAMBLE_MAX_PACKET_SIZE) {
        ESP_LOGW(TAG, "Data too large: %u", (unsigned)ct_len);
        return;
    }

    bramble_header_t rx_hdr;
    if (bramble_header_deserialize(&rx_hdr, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Data packet header invalid");
        return;
    }

    /* Flooding F1 Task 1: dest filter for the ONE flood relay path shared by
     * broadcast and (when s_flood_transport is on) unicast DATA not
     * addressed to us. dest_is_broadcast is always flood-eligible (Task 5,
     * unchanged); dest_is_self never relays (it is delivered below instead,
     * hop_limit is irrelevant once a message has arrived). A unicast frame
     * for someone else only enters the relay block when the toggle is on;
     * when it is off, mesh_process_rx_packet's PKT_TYPE_DATA case never
     * calls handle_data for that frame at all (it calls forward_data_packet,
     * the reactive route-lookup path), so this flag is a belt-and-suspenders
     * check, not the only gate. */
    bool dest_is_broadcast = (rx_hdr.dest_addr == 0xFFFFFFFF);
    bool dest_is_self = (rx_hdr.dest_addr == s_identity->address);

    /* Task 5: multi-hop channel flood. A broadcast/channel DATA (dest ==
     * 0xFFFFFFFF) is "delivered" locally below regardless of whether THIS
     * node can decrypt it (public broadcast vs. a secret channel this node
     * may not belong to) -- that is orthogonal to whether it should be
     * relayed onward. Relaying does not require decrypting: mesh_process_
     * rx_packet already verified the frame's network-key auth_hmac before
     * ever reaching data_rx_decide/handle_data (routing_auth.h), so this
     * frame is known to come from a genuine network-key holder regardless
     * of decrypt outcome. Deliberately placed BEFORE the decrypt fork
     * below, so a node without this channel's key still relays the exact
     * ciphertext onward for members further out in the mesh -- the entire
     * point of a flood is that relays do not need to understand the
     * payload, exactly like RREQ/RERR relays never decrypt anything.
     *
     * Task 1 extends this same block to unicast: when s_flood_transport is
     * on and this frame is unicast for someone else, it goes through the
     * identical dedup + channel_flood_decide + rebroadcast dance as a
     * broadcast flood, reusing channel_flood_decide rather than a second
     * flood implementation. The only difference from broadcast is that a
     * relayed-only unicast frame returns right after this block (see the
     * "not for us" check below) instead of falling into the decrypt/deliver
     * code, since a relay is never the intended recipient. */
    if (dest_is_broadcast || (s_flood_transport && !dest_is_self)) {
        /* Src_addr-qualified dedup key (s_flood_dedup, not the packet_id-
         * only s_dedup already consulted at the mesh_process_rx_packet
         * dispatch gate): see s_flood_dedup's doc comment for why a plain
         * packet_id key risks a cross-source collision on the flood path. */
        uint32_t flood_key = rx_hdr.packet_id ^ src_addr;
        bool is_dup = dedup_check_and_add(&s_flood_dedup, flood_key, now_ms());

        /* A mesh flood means a relay hears its own originated broadcast
         * echoed back once some neighbor rebroadcasts it; re-relaying that
         * echo would burn airtime without ever helping the message reach
         * anywhere new (we already delivered it locally the instant we
         * sent it). Folded into is_duplicate rather than a separate
         * channel_flood_decide input: from a relay decision's point of
         * view "already seen, nothing to gain by relaying again" is
         * exactly the same rule for both cases. Mirrors data_rx_decide's
         * own src_addr == self_addr guard on reverse-route learning, same
         * self-referential-is-meaningless reasoning. */
        bool is_own_echo = (src_addr == s_identity->address);

        /* Non-mutating pre-check against the real BROADCAST-lane airtime
         * budget (same tier TX_KIND_DATA_BROADCAST debits); the actual
         * mesh_tx() at the jittered send time re-checks and debits for
         * real, so a node that goes from "had budget" to "spent it" before
         * its jitter elapses still yields there too. */
        bool budget_permits = tx_gate_check(len, TX_KIND_DATA_BROADCAST);

        channel_flood_decision_t flood = channel_flood_decide(
            rx_hdr.hop_limit, is_dup || is_own_echo, budget_permits, esp_random());

        if (flood.should_relay) {
            uint8_t relay_buf[BRAMBLE_MAX_PACKET_SIZE];
            memcpy(relay_buf, data, len);

            bramble_header_t relay_hdr = rx_hdr;
            relay_hdr.hop_limit = flood.new_hop_limit;
            bramble_header_serialize(&relay_hdr, relay_buf, HEADER_SIZE);

            /* Wire v4: overwrite prev_hop with OUR OWN address before
             * rebroadcast, exactly like forward_data_packet does for
             * unicast forwards -- relay-mutable/MAC-excluded, so this never
             * touches anything under auth_hmac or the AEAD tag. */
            if (len >= BRAMBLE_DATA_PREV_HOP_OFFSET + 4) {
                memcpy(relay_buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
            }

            ESP_LOGI(TAG,
                     "Channel flood relay from %08" PRIX32 " pkt=%08" PRIX32 " hop_limit %u->%u",
                     src_addr, rx_hdr.packet_id, rx_hdr.hop_limit, flood.new_hop_limit);
            schedule_flood_relay(relay_buf, len, flood.jitter_ms, flood_key,
                                 TX_KIND_DATA_BROADCAST);
        } else if (!budget_permits) {
            ESP_LOGD(TAG, "Channel flood relay denied by airtime budget, pkt=%08" PRIX32,
                     rx_hdr.packet_id);
        }
    }

    /* Task 1: a unicast frame addressed to someone else is a relay-only
     * pass-through under the flood toggle (handled above); it is never
     * decrypted or delivered here (we have no session/channel-key basis for
     * doing so on someone else's behalf, and doing so would risk spurious
     * local-delivery side effects: msg_store, an outgoing ACK, replay-window
     * updates, none of which belong to a frame that is not ours). Broadcast
     * (dest_is_broadcast) and unicast-to-self (dest_is_self) both fall
     * through to the decrypt/deliver code below exactly as before this
     * task. */
    if (!dest_is_broadcast && !dest_is_self) {
        return;
    }

    /* AAD excludes hop_limit (relays decrement it in flight) but binds
     * src_addr (SEC-M2 residual); must match the masked, src-bound AAD the
     * originator built in send_data_packet/send_dm_packet. */
    uint8_t aad[HEADER_SIZE + 4];
    bramble_build_aead_aad(&rx_hdr, src_addr, aad, sizeof(aad));

    uint8_t plaintext[CHANNEL_MSG_MAX_PLAINTEXT_SIZE] = {0};

    /* SEC-C2: FLAG_CHANNEL selects the decrypt mechanism, not just a
     * bookkeeping bit. Set: trial-decrypt under known channel keys (this is
     * also the handshake TRANSPORT, app_type APP_TYPE_KE, since no session
     * exists yet when handshaking). Absent: this is a unicast DM payload,
     * decrypted under the ACTIVE session key for src_addr; there is no
     * channel-key fallback for this path, matching send_dm_packet never
     * setting FLAG_CHANNEL on the way out. */
    int is_channel_message = (rx_hdr.flags & FLAG_CHANNEL) ? 1 : 0;
    if (is_channel_message) {
        int ret = channel_msg_decrypt(s_channels, s_num_channels, nonce, ciphertext, ct_len, tag,
                                      aad, HEADER_SIZE + 4, plaintext, &info, now_ms());
        if (ret != 0) {
            ESP_LOGW(TAG, "Failed to decrypt channel data from %08" PRIX32, src_addr);
            return;
        }
    } else {
        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(s_dm_table, src_addr);
        int drc = DM_DECRYPT_FAIL; /* no active session -> treat as a desync */
        size_t dm_pt_len = 0;
        if (sess && sess->state == DM_STATE_ACTIVE) {
            /* Ratchet decrypt: reads the cleartext epoch||index header (front of
             * ciphertext), derives exactly that one message key (caching skipped
             * keys, bounded by DM_MAX_SKIP), and does a single GCM decrypt. */
            drc = dm_session_ratchet_decrypt(sess, &rx_hdr, src_addr, nonce, ciphertext, ct_len,
                                             tag, plaintext, &dm_pt_len);
            if (drc == DM_DECRYPT_OK)
                sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
        }
        DM_MUTEX_GIVE();
        if (drc == DM_DECRYPT_REPLAY) {
            /* An already-seen index: drop silently, never heal or deliver. (The
             * ratchet never returns this today; the authoritative replay defense
             * is the per-sender nonce window below. Handled for completeness so a
             * future replay signal cannot masquerade as a desync.) */
            ESP_LOGD(TAG, "DM replay drop from %08" PRIX32, src_addr);
            return;
        }
        if (drc != DM_DECRYPT_OK) {
            ESP_LOGW(TAG, "Failed session decrypt from %08" PRIX32, src_addr);
            /* Our DM session with this peer has desynced (no session, a stale key
             * it no longer holds, or a ratchet index beyond the skip bound,
             * DM_DECRYPT_TOO_FAR). Silently returning here is what made DMs fail
             * permanently after one side rebooted; instead, kick a rate-limited
             * re-handshake so the session self-heals (the ratchet adds no new
             * recovery mechanism, it degrades into this existing one). */
            maybe_trigger_dm_rehandshake(src_addr);
            return;
        }
        /* Session payloads are always chat in this wiring: there is no
         * session-encrypted handshake message (handshakes always ride the
         * channel key, since no session exists yet by definition), and
         * session sends never fragment (mesh_send_dm rejects payloads over
         * FRAG_MAX_PLAINTEXT rather than fragmenting under a session key).
         * channel_id/channel_index are left at their zero-value (not a
         * channel message); the RPC onMessage notification below already
         * renders channel_id==0 as "channel": -1. */
        memset(&info, 0, sizeof(info));
        info.app_type = APP_TYPE_CHAT;
        info.src_addr = src_addr;
        info.data = plaintext;
        info.data_len = dm_pt_len;
    }

    /* SEC-M1: authenticated replay protection, keyed on the nonce counter
     * (Task 0.4) rather than the cleartext packet_id, so it cannot be
     * spoofed by an attacker who doesn't hold the relevant key. Runs only
     * after a successful decrypt, and only when src_addr is trustworthy
     * (Fix 2, red-team panel): BRAMBLE_PUBLIC_CHANNEL_PSK is public, so a
     * public-channel decrypt's src_addr is a free-to-forge claim, not
     * "bound to this sender's identity". Feeding it into the SHARED
     * per-sender window would let an attacker claim src_addr=victim and
     * slam the victim's high-water mark, dropping the victim's own later,
     * genuine packets as BELOW_WINDOW (a mesh-wide DoS). Public-channel
     * traffic relies on the pre-existing packet_id/type dedup for loop
     * suppression instead; a secret channel's or a session's src_addr is
     * still authenticated (channel membership or a session key) and still
     * goes through the window as before. */
    uint64_t rx_counter = nonce_counter_extract(nonce);
    if (channel_source_is_replay_trustworthy(is_channel_message, info.channel_index)) {
        int rp = replay_check_and_add(&s_replay, src_addr, rx_counter, now_ms());
        /* Drop on anything that is not an outright accept or the one code
         * with a legitimate tier-2 continuation below. Written as an
         * allow-list rather than `== REPLAY_REJECT_DUP` so that a future
         * reject code (REPLAY_REJECT_NO_SLOT was exactly this case) cannot
         * fall through this block and be treated as accepted. */
        if (rp != REPLAY_ACCEPT && rp != REPLAY_BELOW_WINDOW) {
            ESP_LOGD(TAG, "Replay drop from %08" PRIX32 " ctr=%llu (rp=%d)", src_addr,
                     (unsigned long long)rx_counter, rp);
            return;
        }
        /* Issue #72: apply the authenticated sent_at freshness bound on the
         * ACCEPT path too, not just the below-window path. Persistence
         * bounds the post-reboot exposure to one flush interval; this
         * closes that residual for CHAT, which is the only app type
         * carrying a sent_at inside the ciphertext. Gated on confident
         * timesync because without trusted time the bound is unevaluable,
         * and an unevaluable bound must not reject live traffic (tier-1
         * acceptance is not the fail-closed layer, tier-2 is). */
        /* Ordering note (#163): replay_check_and_add above has already
         * advanced high_water/window/dirty for this counter by the time
         * the freshness check below can still drop the packet. That is
         * intentional, not a bug. This code only runs post-decrypt (see
         * the comment above channel_source_is_replay_trustworthy), so
         * rx_counter already passed AEAD authentication: it is a counter
         * value the sender actually issued, not one an attacker forged.
         * A stale-but-above-high-water replay (for example one delivered
         * before a reboot but after the last NVS flush) therefore
         * self-heals the post-reboot flush-lag gap using the attacker's
         * own replay attempt. The message is still dropped below and
         * never delivered, and the window can never be pushed past the
         * sender's real highest-ever counter, because the attacker
         * cannot mint new ones. Do not reorder the freshness check ahead
         * of replay_check_and_add: that would reopen the flush-lag window
         * this comment describes. */
        if (rp == REPLAY_ACCEPT && info.app_type == APP_TYPE_CHAT &&
            timesync_is_confident(&s_timesync, now_ms())) {
            uint32_t now_s = (uint32_t)(timesync_get_network_time(&s_timesync, now_ms()) / 1000);
            if (info.sent_at > now_s + DEFERRED_SKEW_S ||
                (now_s > info.sent_at && (now_s - info.sent_at) > DEFERRED_TTL_S)) {
                ESP_LOGD(TAG, "Stale chat drop from %08" PRIX32 " ctr=%llu sent_at=%" PRIu32,
                         src_addr, (unsigned long long)rx_counter, info.sent_at);
                return;
            }
        }
        /* Fix 3 (red-team panel): a CHAT counter accepted here via tier-1
         * must also be recorded in the tier-2 deferred cache. Without
         * this, a counter that later ages out of the 64-entry tier-1
         * window is in NEITHER dedup structure: a captured, genuinely
         * delivered chat packet replays successfully once the window has
         * moved past it (tier-1 reads BELOW_WINDOW, and a tier-2 cache
         * that was never told about the original acceptance treats the
         * replay as a fresh, legitimate deferred delivery). Only chat
         * carries the authenticated sent_at that replay_deferred_accept
         * needs, and it is the only app type ever deferred, so there is
         * nothing to record for any other app_type. */
        if (rp == REPLAY_ACCEPT && info.app_type == APP_TYPE_CHAT) {
            uint32_t now_s = (uint32_t)(timesync_get_network_time(&s_timesync, now_ms()) / 1000);
            replay_deferred_mark_seen(&s_deferred, src_addr, rx_counter, now_s);
        }
        /* Tier-2 deferred acceptance (Task 0.6): a chat message can
         * legitimately arrive outside the tier-1 sliding window (long
         * store-and-forward delay), but only chat carries an authenticated
         * sent_at to bound how old is too old. now_s uses network time
         * (degrading to local uptime pre-sync) so it is on the same clock
         * basis as the sender's sent_at; timesync_is_confident gates this
         * fail-closed under untrusted time (NEW-SEC-4). */
        if (rp == REPLAY_BELOW_WINDOW) {
            if (info.app_type != APP_TYPE_CHAT) {
                return;
            }
            uint32_t now_s = (uint32_t)(timesync_get_network_time(&s_timesync, now_ms()) / 1000);
            int dp = replay_deferred_accept(&s_deferred, src_addr, rx_counter, info.sent_at, now_s,
                                            timesync_is_confident(&s_timesync, now_ms()));
            if (dp != REPLAY_ACCEPT) {
                ESP_LOGD(TAG, "Deferred replay drop from %08" PRIX32 " ctr=%llu sent_at=%" PRIu32,
                         src_addr, (unsigned long long)rx_counter, info.sent_at);
                return;
            }
        }
    }

    /* APP_TYPE_KE (handshake-in-DATA, SEC-C2): parse+dispatch and return,
     * never reaching the chat/fragment logic below. Runs after tier-1
     * replay above, so a replayed handshake nonce is still caught by the
     * same authenticated window every other DATA type uses; the
     * handshake-level dedup (hs_dedup_check_and_record, keyed on the
     * message content rather than the nonce) is a separate check inside
     * handle_ke_envelope. Only reachable via the FLAG_CHANNEL branch above
     * (a session payload is always built with app_type == APP_TYPE_CHAT). */
    if (info.app_type == APP_TYPE_KE) {
        handle_ke_envelope(src_addr, info.channel_index, info.data, info.data_len);
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

            /* Validate fragment header: indices < total and total within limits */
            if (frag_hdr.frag_total > 1 && frag_hdr.frag_index < frag_hdr.frag_total &&
                frag_hdr.frag_total <= FRAG_MAX_FRAGMENTS) {
                /* This is a fragment: process through reassembly */
                ESP_LOGI(TAG, "RX fragment %u/%u msg_id=%04X from %08" PRIX32,
                         frag_hdr.frag_index + 1, frag_hdr.frag_total, frag_hdr.message_id,
                         info.src_addr);

                int ret =
                    reassembly_add(&s_reassembly, &frag_hdr, info.data + FRAG_HEADER_SIZE,
                                   info.data_len - FRAG_HEADER_SIZE, now_ms(), rx_hdr.packet_id);
                if (ret == 1) {
                    /* Reassembly complete: collect the full message.
                     * Buffers allocated on heap to avoid ~1.2KB stack pressure
                     * in the mesh task (which has tight stack headroom). */
                    /* Get first-received fragment's packet_id before collect frees slot */
                    uint32_t first_frag_pkt_id =
                        reassembly_get_first_packet_id(&s_reassembly, frag_hdr.message_id);

                    size_t reasm_sz =
                        frag_hdr.frag_total * FRAG_MAX_PLAINTEXT; /* F27: size to actual count */
                    uint8_t* reassembled = malloc(reasm_sz);
                    if (!reassembled) {
                        ESP_LOGE(TAG, "OOM for reassembly buffer");
                        return;
                    }
                    int total_len = reassembly_collect(&s_reassembly, frag_hdr.message_id,
                                                       reassembled, reasm_sz);
                    if (total_len > 0) {
                        /* Process the reassembled message */
                        char* text = malloc(reasm_sz + 1);
                        if (!text) {
                            ESP_LOGE(TAG, "OOM for reassembly text buffer");
                            free(reassembled);
                            return;
                        }
                        size_t tlen = (size_t)total_len;
                        if (tlen >= reasm_sz + 1)
                            tlen = reasm_sz;
                        memcpy(text, reassembled, tlen);
                        text[tlen] = '\0';

                        ESP_LOGI(TAG, "");
                        ESP_LOGI(TAG, "*** REASSEMBLED MESSAGE from %08" PRIX32 " ***",
                                 info.src_addr);
                        ESP_LOGI(TAG, ">>> %s", text);
                        ESP_LOGI(TAG, "*** (%u bytes from %u fragments, ch:%d RSSI:%d SNR:%d) ***",
                                 (unsigned)total_len, frag_hdr.frag_total, info.channel_id, rssi,
                                 snr);

                        /* Store and notify for reassembled message */
                        uint32_t hdr_dest;
                        memcpy(&hdr_dest, data + 4, 4);
                        bool is_channel_message = (info.channel_id > 0);
                        msg_direction_t dir = (hdr_dest == 0xFFFFFFFF && !is_channel_message)
                                                  ? MSG_DIR_BROADCAST_IN
                                                  : MSG_DIR_INCOMING;
                        if (is_channel_message) {
                            msg_store_add_channel(info.src_addr, dir, text, tlen, rssi, snr, 0,
                                                  MSG_STATUS_NONE, (uint8_t)info.channel_id);
                        } else {
                            /* channel_id 0: a DM or a received broadcast. Both store
                             * channel-less (MSG_STORE_DM_CHANNEL); the direction, not a
                             * channel index, files a broadcast into the Broadcast thread. */
                            msg_store_add_dm(info.src_addr, dir, text, tlen, rssi, snr, 0,
                                             MSG_STATUS_NONE);
                        }

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

                            cJSON* params = cJSON_CreateObject();
                            cJSON_AddStringToObject(params, "from", addr_buf);
                            /* Sender display name when the neighbor table
                             * knows it (from beacons): clients then never
                             * have to show a bare hex address for a peer
                             * this node has already heard from. */
                            {
                                neighbor_entry_t* nb = neighbor_lookup(&s_neighbors, info.src_addr);
                                if (nb && nb->name[0] != '\0') {
                                    cJSON_AddStringToObject(params, "fromName", nb->name);
                                }
                            }
                            cJSON_AddStringToObject(params, "text", text);
                            cJSON_AddNumberToObject(params, "rssi", rssi);
                            cJSON_AddNumberToObject(params, "snr", snr);
                            cJSON_AddNumberToObject(params, "channel",
                                                    msg_store_rx_channel_index(info.channel_id));
                            cJSON_AddBoolToObject(params, "broadcast",
                                                  (dir == MSG_DIR_BROADCAST_IN));
                            rpc_notify("bramble.onMessage", params);
                            cJSON_Delete(params);
                        }

                        /* Reassembled message: the broadcast receipt reports the
                         * first-received fragment's packet_id. */
                        confirm_data_delivery(dir, info.src_addr, src_addr, rx_hdr.dest_addr,
                                              rx_hdr.packet_id, first_frag_pkt_id, rssi);

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

        /* Not a fragment: process as regular single-packet message */
        char text[256];
        size_t tlen = info.data_len;
        if (tlen >= sizeof(text))
            tlen = sizeof(text) - 1;
        memcpy(text, info.data, tlen);
        text[tlen] = '\0';

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "*** MESSAGE from %08" PRIX32 " ***", info.src_addr);
        ESP_LOGI(TAG, ">>> %s", text);
        ESP_LOGI(TAG, "*** (ch:%d RSSI:%d SNR:%d) ***", info.channel_id, rssi, snr);

        /* Store in message store: classify broadcast vs channel routing */
        uint32_t hdr_dest;
        memcpy(&hdr_dest, data + 4, 4); /* dest_addr at offset 4 in header */
        bool is_channel_message = (info.channel_id > 0);
        msg_direction_t dir = (hdr_dest == 0xFFFFFFFF && !is_channel_message) ? MSG_DIR_BROADCAST_IN
                                                                              : MSG_DIR_INCOMING;
        if (is_channel_message) {
            msg_store_add_channel(info.src_addr, dir, text, tlen, rssi, snr, 0, MSG_STATUS_NONE,
                                  (uint8_t)info.channel_id);
        } else {
            /* channel_id 0: a DM or a received broadcast. Both store channel-less
             * (MSG_STORE_DM_CHANNEL); the direction, not a channel index, files a
             * broadcast into the Broadcast thread. */
            msg_store_add_dm(info.src_addr, dir, text, tlen, rssi, snr, 0, MSG_STATUS_NONE);
        }

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

            cJSON* params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "from", addr_buf);
            /* Sender display name when the neighbor table knows it. */
            {
                neighbor_entry_t* nb = neighbor_lookup(&s_neighbors, info.src_addr);
                if (nb && nb->name[0] != '\0') {
                    cJSON_AddStringToObject(params, "fromName", nb->name);
                }
            }
            cJSON_AddStringToObject(params, "text", text);
            cJSON_AddNumberToObject(params, "rssi", rssi);
            cJSON_AddNumberToObject(params, "snr", snr);
            cJSON_AddNumberToObject(params, "channel", msg_store_rx_channel_index(info.channel_id));
            cJSON_AddBoolToObject(params, "broadcast", dir == MSG_DIR_BROADCAST_IN);
            rpc_notify("bramble.onMessage", params);
            cJSON_Delete(params);
        }

        /* Single packet: ACK and receipt are keyed on the packet's own id. */
        confirm_data_delivery(dir, info.src_addr, src_addr, rx_hdr.dest_addr, rx_hdr.packet_id,
                              rx_hdr.packet_id, rssi);

        /* Also print to stdout for CLI users */
        printf("\n[MSG from %08" PRIX32 "] %s\n", info.src_addr, text);
        printf("bramble> ");
        fflush(stdout);
    }
}

/* ── Routing packet handlers ────────────────────────────────────────── */

/**
 * The only way mesh_task puts bytes on the air. Wraps the tx_gate
 * chokepoint (budget check -> LBT/CAD -> transmit -> ToA debit) with
 * mesh-side bookkeeping: TX telemetry, packet counters, and the shared
 * airtime snapshot for the UI/RPC.
 *
 * Returns TX_GATE_OK, TX_GATE_ERR_BUDGET (denied, nothing transmitted),
 * TX_GATE_ERR_CHANNEL_BUSY (LBT deferred, nothing transmitted) or
 * TX_GATE_ERR_RADIO. Per-kind deny behavior lives at the call sites.
 */
int mesh_tx(const uint8_t* buf, uint8_t len, tx_kind_t kind) {
    uint8_t pkt_type = (len >= 2) ? buf[1] : 0xFF;

    /* Relaxed read: neighbor_count(&s_neighbors) may run from the RPC/UI
     * task without the mesh state mutex (pre-existing pattern; the old
     * airtime_budget_set_mesh_size call sites did the same). Worst case
     * is a momentarily stale peer count selecting an adjacent budget
     * profile; the gate re-reads it on the next transmission. */
    tx_gate_set_peer_count((uint8_t)neighbor_count(&s_neighbors));
    int rc = tx_gate_send(buf, len, kind);
    if (rc == TX_GATE_OK) {
        traffic_debug_record_tx(&s_traffic_debug, pkt_type, len, tx_gate_kind_tier(kind));

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.packets_tx++;
        tx_gate_snapshot(&s_shared.airtime);
        xSemaphoreGive(s_state_mutex);
    } else if (rc == TX_GATE_ERR_BUDGET) {
        ESP_LOGD(TAG, "TX denied by airtime budget (kind=%d type=0x%02X len=%u)", (int)kind,
                 pkt_type, len);
    }
    return rc;
}

/* ── End jittered channel-flood relay ───────────────────────────── */

/* uid: 0 = brand new user message (allocate a uid and store its one row);
 * nonzero = a row for this message already exists, UPDATE it, never add. */
static uint32_t mesh_send_message_uid(uint32_t dest_addr, const uint8_t* data, size_t len,
                                      uint32_t uid);

void flush_queued_messages(uint32_t dest_addr) {
    /* Route-established trigger only. QUEUE_REASON_SESSION entries are
     * flushed separately by flush_session_queue on session establishment;
     * touching them here would re-run mesh_send_message's route+session
     * decision and double-queue them under a fresh slot.
     *
     * The entry's uid rides back into the send path so the re-send reconciles
     * the pending row this message already owns instead of storing a second
     * one (the duplicate-bubble bug). */
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (s_queued_msgs[i].used && s_queued_msgs[i].reason == QUEUE_REASON_ROUTE &&
            s_queued_msgs[i].dest_addr == dest_addr) {
            ESP_LOGI(TAG, "Sending queued msg to %08" PRIX32 " (%u bytes)", dest_addr,
                     (unsigned)s_queued_msgs[i].len);
            mesh_send_message_uid(dest_addr, s_queued_msgs[i].data, s_queued_msgs[i].len,
                                  s_queued_msgs[i].uid);
            s_queued_msgs[i].used = false;
        }
    }
}

static void mesh_process_rx_packet(const rx_packet_t* pkt) {
    if (pkt->len < HEADER_SIZE) {
        ESP_LOGW(TAG, "Packet too short: %u bytes", pkt->len);
        return;
    }

    bramble_header_t header;
    if (bramble_header_deserialize(&header, pkt->data, pkt->len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid header");
        return;
    }

    if (!bramble_header_is_supported_version(&header)) {
        /* Version flag day (ground rule 10): un-upgraded nodes are off-network.
         * Rate-limited so a half-updated fleet is diagnosable, not silent. */
        static uint32_t s_last_ver_log_ms = 0;
        uint32_t vnow = now_ms();
        if (vnow - s_last_ver_log_ms > 60000) {
            ESP_LOGW(TAG, "Dropping wire v%u packet (need v%u)", header.version, BRAMBLE_VERSION);
            s_last_ver_log_ms = vnow;
        }
        return;
    }

    /* Record raw RX event. The claimed origin rides along so an RSSI sample
     * can be attributed to a peer: neighbor-table RSSI only refreshes on
     * beacon reception, so without this the event stream carries signal
     * strength that belongs to nobody in particular. Unknown stays 0. */
    uint32_t traffic_src = 0;
    bramble_packet_origin_addr(header.type, pkt->data, pkt->len, &traffic_src);
    traffic_debug_record_rx(&s_traffic_debug, header.type, pkt->len, pkt->rssi, traffic_src);

    /* Dedup check:
     * - include packet type to avoid PROBE vs PROBE_ACK collisions
     * - for PROBE_ACK, include responder addr so multiple peers can respond
     *   to the same probe_id without being collapsed as duplicates.
     *
     * SEC-M1: authenticated replay of DATA/LOCATION is enforced post-decrypt
     * via replay_check_and_add on the nonce counter; this dedup only
     * suppresses relay loops of unauthenticated control packets.
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
        maybe_emit_implicit_broadcast_delivery(&header, pkt);

        /* Task 6 (GAP A): a duplicate unicast DATA addressed to us is the
         * sender's retransmit of an already-delivered message (its first
         * ACK was lost in transit). Re-send the ACK (idempotent, gives the
         * retransmit's sender another chance to hear the confirmation)
         * WITHOUT touching handle_data's decrypt/deliver path, so local
         * delivery stays exactly-once. src_addr is read directly off the
         * still-plaintext wire prefix here (SEC-M2); s_delivered_dedup only
         * ever gains an entry AFTER a frame with that exact (src_addr,
         * packet_id) pair already passed full auth_hmac verification,
         * decrypt, and local delivery (handle_data), so a hit here can only
         * be produced by replaying bytes that already cleared that bar
         * once. Finding 3 (final whole-branch review): for consistency with
         * the Task-5 lesson of never acting on unauthenticated wire bytes,
         * the re-ACK itself is additionally gated on data_auth_verify here
         * (same network-key MAC check the PKT_TYPE_DATA case below runs),
         * so a re-ACK can only be triggered by a frame that is ALSO a valid,
         * currently-authenticated DATA frame, not merely one whose src_addr/
         * packet_id happen to collide with a past delivery. On auth
         * failure this falls through to the normal duplicate-drop below
         * (no ACK, no re-verification retry) exactly as if this dup-ACK
         * carve-out didn't exist. Length-guarded the same way the
         * PKT_TYPE_DATA case guards its own data_auth_verify call, since
         * auth_hmac lives at BRAMBLE_DATA_AUTH_HMAC_OFFSET..NONCE_OFFSET. */
        if (header.type == PKT_TYPE_DATA && header.dest_addr == s_identity->address &&
            pkt->len >= BRAMBLE_DATA_NONCE_OFFSET) {
            uint32_t dup_src_addr;
            memcpy(&dup_src_addr, pkt->data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
            uint32_t delivered_key = header.packet_id ^ dup_src_addr;
            if (dedup_contains(&s_delivered_dedup, delivered_key, now_ms()) &&
                data_auth_verify(&header, dup_src_addr,
                                 pkt->data + BRAMBLE_DATA_AUTH_HMAC_OFFSET)) {
                ESP_LOGI(TAG,
                         "Re-sending ACK for already-delivered duplicate pkt=%08" PRIX32
                         " from %08" PRIX32,
                         header.packet_id, dup_src_addr);
                send_ack(dup_src_addr, header.packet_id, pkt->rssi);
            }
        }

        /* Flooding F1 rebroadcast suppression: the dispatch dedup gate above
         * catches the 2nd/3rd... copies of a flooded DATA frame and returns
         * BEFORE handle_data, so this is the one place a node can COUNT the
         * OTHER relays it overhears while its own rebroadcast still waits out
         * its jitter. On a duplicate DATA that is on the flood relay path
         * (broadcast, or -- under s_flood_transport -- unicast for someone
         * else), find the matching queued relay by its src-qualified key
         * (packet_id ^ src_addr, recomputed here from the wire the same way
         * schedule_flood_relay recorded it) and register the overheard copy;
         * channel_flood_note_overheard cancels the relay once FLOOD_SUPPRESS_
         * AFTER copies are in. This is disjoint from the re-ACK carve-out
         * above, which only fires for unicast-to-SELF duplicates. */
        bool flood_relay_active = (header.dest_addr == 0xFFFFFFFF ||
                                   (s_flood_transport && header.dest_addr != s_identity->address));
        if (header.type == PKT_TYPE_DATA && flood_relay_active &&
            pkt->len >= BRAMBLE_DATA_NONCE_OFFSET) {
            uint32_t flood_dup_src;
            memcpy(&flood_dup_src, pkt->data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
            /* Finding (final whole-branch review): only an AUTHENTICATED
             * duplicate copy may count toward suppression. The first
             * legitimate copy inserts the s_dedup key ABOVE (dedup_check_and_
             * add) BEFORE the network-key MAC is ever checked, so without this
             * gate a keyless party could replay a garbage-MAC frame carrying a
             * matching plaintext packet_id + src_addr, hit this dedup branch,
             * and bump `heard` to FLOOD_SUPPRESS_AFTER -- cancelling a genuine
             * node's pending relay and punching a targeted coverage hole in a
             * sparse mesh. Verify the copy's network-key MAC first, mirroring
             * the re-ACK carve-out above and handle_data's own data_auth_verify
             * gate, so only genuine overheard copies suppress. Costs one HMAC
             * per overheard flood duplicate (precedented, acceptable). The
             * length guard is widened to BRAMBLE_DATA_NONCE_OFFSET so the 8
             * auth_hmac bytes at BRAMBLE_DATA_AUTH_HMAC_OFFSET are in range. */
            if (data_auth_verify(&header, flood_dup_src,
                                 pkt->data + BRAMBLE_DATA_AUTH_HMAC_OFFSET)) {
                uint32_t flood_dup_key = header.packet_id ^ flood_dup_src;
                if (channel_flood_note_overheard(s_flood_relay_queue, FLOOD_RELAY_QUEUE_CAPACITY,
                                                 flood_dup_key)) {
                    ESP_LOGD(TAG,
                             "Flood relay suppressed after %d overheard copies, pkt=%08" PRIX32,
                             FLOOD_SUPPRESS_AFTER, header.packet_id);
                }
            }
        }

        /* Flooding F1 Task 2: the same suppression bookkeeping for a flooded
         * ACK. A flooded ACK addressed to someone else (i.e. NOT consumed by
         * this node) is relayed via handle_ack's flood branch, which queues a
         * jittered rebroadcast keyed on ack.header.packet_id ^ ack.src_addr.
         * The 2nd/3rd... copies land here at the dispatch dedup gate; recompute
         * that same key from the wire and register the overheard copy so the
         * pending ACK relay cancels once FLOOD_SUPPRESS_AFTER copies are in.
         * ACK.src_addr is big-endian on the wire at HEADER_SIZE (packet.c's
         * put_be32), unlike DATA's little-endian src_addr, so it is read
         * big-endian here to match handle_ack's host-order ack.src_addr. */
        if (header.type == PKT_TYPE_ACK && s_flood_transport &&
            header.dest_addr != s_identity->address && pkt->len >= HEADER_SIZE + 4) {
            /* Finding (final whole-branch review): as with the DATA flood
             * above, only an AUTHENTICATED overheard copy may count toward
             * suppression. handle_ack inserts the s_dedup key at the dispatch
             * gate BEFORE ack_verify runs, so without this gate a garbage-MAC
             * duplicate carrying a matching plaintext packet_id + src_addr
             * would reach this counter and cancel a genuine node's pending ACK
             * relay. Deserialize and verify the network-key MAC here, mirroring
             * handle_ack's ack_verify gate, so only genuine copies suppress.
             * dup_ack.src_addr (host order) equals the big-endian wire read
             * this block previously did by hand (proven by
             * test_flood_ack_wire_suppression_key_matches). Costs one
             * deserialize + HMAC per overheard flooded-ACK duplicate. */
            bramble_ack_t dup_ack;
            if (bramble_ack_deserialize(&dup_ack, pkt->data, pkt->len) == ESP_OK &&
                ack_verify(&dup_ack)) {
                uint32_t ack_dup_key = dup_ack.header.packet_id ^ dup_ack.src_addr;
                if (channel_flood_note_overheard(s_flood_relay_queue, FLOOD_RELAY_QUEUE_CAPACITY,
                                                 ack_dup_key)) {
                    ESP_LOGD(
                        TAG,
                        "Flooded ACK relay suppressed after %d overheard copies, pkt=%08" PRIX32,
                        FLOOD_SUPPRESS_AFTER, header.packet_id);
                }
            }
        }

        /* Per-node identity Phase 3: the same suppression bookkeeping for a
         * flooded identity attestation. Copies 2+ of a relayed attestation
         * land here at the dispatch dedup gate (same packet_id: the frame
         * relays unmodified except hop_limit); count each AUTHENTICATED
         * copy against any pending relay of ours so it cancels after
         * FLOOD_SUPPRESS_AFTER overheard copies. Mirrors the flooded-ACK
         * block above, including the MAC-before-counting rule: without
         * ident_relay_verify here a keyless party could replay a
         * garbage-MAC copy to cancel a genuine relay and punch a coverage
         * hole. Relays still never Ed25519-verify; this is the cheap MAC
         * only. */
        if (header.type == PKT_TYPE_IDENTITY_ATTESTATION) {
            bramble_identity_attestation_t dup_att;
            if (bramble_identity_attestation_deserialize(&dup_att, pkt->data, pkt->len) == ESP_OK &&
                ident_relay_verify(&dup_att)) {
                uint32_t att_dup_key = dup_att.header.packet_id ^ dup_att.src_addr;
                if (channel_flood_note_overheard(s_flood_relay_queue, FLOOD_RELAY_QUEUE_CAPACITY,
                                                 att_dup_key)) {
                    ESP_LOGD(TAG,
                             "Attestation relay suppressed after %d overheard copies,"
                             " pkt=%08" PRIX32,
                             FLOOD_SUPPRESS_AFTER, header.packet_id);
                }
            }
        }

        ESP_LOGD(TAG, "Duplicate packet key=%08" PRIX32 " (pkt=%08" PRIX32 " type=0x%02X)",
                 dedup_key, header.packet_id, header.type);
        /* Note: dedup drop already recorded in initial RX event - no separate event needed */
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.packets_rx++;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "RX type=0x%02X from pkt_id=%08" PRIX32 " RSSI:%d SNR:%d", header.type,
             header.packet_id, pkt->rssi, pkt->snr);

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
    case PKT_TYPE_DATA: {
        /* components/routing/forwarding.c: data_rx_decide() owns the
         * deliver-locally-vs-forward fork (Task 3, ws 1.5) AND, as of wire
         * v4 (Task 4), the reverse-route-learning decision: every DATA
         * frame we receive or forward teaches us a route back to its
         * originator (src_addr) via prev_hop, the verified last radio hop.
         * That is what leaves a breadcrumb route at every relay on the
         * forward path, so the destination's ACK/receipt has somewhere to
         * go home to instead of dying at route_lookup(src_addr) == NULL.
         *
         * src_addr/prev_hop are read directly off the wire here (both are
         * plaintext outside the AEAD ciphertext; see packet.h). A frame too
         * short to hold the full envelope prefix (through the auth_hmac and
         * up to the nonce) is dropped outright: there is no valid v4
         * DATA/LOCATION frame this short. */
        if (pkt->len < BRAMBLE_DATA_NONCE_OFFSET) {
            ESP_LOGW(TAG, "DATA frame too short for v4 envelope: %u", pkt->len);
            break;
        }
        uint32_t data_src_addr;
        uint32_t data_prev_hop;
        memcpy(&data_src_addr, pkt->data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
        memcpy(&data_prev_hop, pkt->data + BRAMBLE_DATA_PREV_HOP_OFFSET, 4);

        /* Task 4-fix F1 (Critical): authenticate the frame's origin BEFORE
         * learning any reverse route or forwarding. A relay never decrypts
         * DATA, so the AEAD tag cannot gate this; the network-key auth_hmac
         * over the masked header + src_addr is the DATA analogue of
         * RREP/ACK/RERR's control-plane MAC. A keyless attacker (no network
         * key) cannot forge it, so it can no longer inject a frame that
         * poisons every node's route toward a spoofed victim. On failure we
         * neither learn nor forward nor deliver -- a bad-MAC DATA frame is
         * treated exactly like any unauthenticated control frame. A
         * legitimate frame always carries a valid MAC (every originator
         * signs; all provisioned nodes share the key), so this drops nothing
         * real. When unprovisioned there is no key, so the verify fails
         * closed (all-zero sentinel, rejected before compare) and the node is
         * inert; only a keyed insider can forge (the remaining residual). */
        if (!data_auth_verify(&header, data_src_addr, pkt->data + BRAMBLE_DATA_AUTH_HMAC_OFFSET)) {
            ESP_LOGW(TAG, "DATA auth_hmac failed (src=%08" PRIX32 " prev_hop=%08" PRIX32 "), drop",
                     data_src_addr, data_prev_hop);
            break;
        }

        /* Liveness from traffic, not just beacons. prev_hop == src_addr means
         * the originator put this frame on the air itself, and src_addr is
         * MAC-covered by the data_auth_verify above, so this is an
         * authenticated "that peer is alive right now" without leaning on the
         * relay-mutable prev_hop hint (a relayed frame teaches us nothing
         * authenticated about who transmitted it, so it is skipped). Beacon
         * cadence alone left a peer we were actively talking to reading as
         * minutes stale, and eventually purged mid-conversation. */
        if (data_prev_hop == data_src_addr) {
            mesh_note_peer_heard(data_src_addr, pkt->rssi, pkt->snr);
        }

        /* Metric mirrors handle_rrep's pattern (metric_apply_link_penalty
         * computed by the caller, then passed into the pure decide
         * function): DATA carries no accumulated path metric of its own,
         * so the base is the same maximum (255) RREQ originates with,
         * penalized by the one link this frame was just heard on. */
        uint8_t data_link_metric = metric_apply_link_penalty(255, (int8_t)pkt->rssi, pkt->snr);
        data_rx_decision_t data_rx =
            data_rx_decide(header.dest_addr, s_identity->address, data_src_addr, data_prev_hop,
                           header.hop_limit, data_link_metric);

        if (data_rx.install_reverse_route) {
            /* Task 4-fix F2: breadcrumbs install as ROUTE_SRC_BREADCRUMB so
             * they can never displace an HMAC-gated DISCOVERED route (and a
             * later DISCOVERED route always reclaims this entry). */
            route_install(&s_routes, data_rx.reverse_dest, data_rx.reverse_next_hop,
                          data_rx.reverse_hop_count, data_rx.reverse_metric, ROUTE_ACTIVE,
                          ROUTE_SRC_BREADCRUMB, now_ms());
        }

        /* Flooding F1 Task 1: DATA_RX_FORWARD means this is unicast for
         * someone else. Reactive (s_flood_transport off, default): unchanged
         * route-lookup forward. Flood (on): route it through handle_data
         * instead, which now relays it via the shared flood engine
         * (channel_flood_decide) rather than looking up a route; it never
         * calls forward_data_packet in flood mode. DATA_RX_DELIVER (dest ==
         * self or broadcast) is unaffected by the toggle: always handle_data. */
        if (data_rx.action == DATA_RX_FORWARD) {
            if (s_flood_transport) {
                handle_data(pkt->data, pkt->len, pkt->rssi, pkt->snr);
            } else {
                forward_data_packet(pkt->data, pkt->len, &header);
            }
        } else {
            handle_data(pkt->data, pkt->len, pkt->rssi, pkt->snr);
        }
        break;
    }
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
    case PKT_TYPE_IDENTITY_ATTESTATION:
        /* Phase 3: verify (cheap MAC first) + TOFU-pin + flood relay.
         * See handle_identity_attestation for the full order contract. */
        handle_identity_attestation(pkt->data, pkt->len);
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
static esp_err_t mesh_init_radio_config(radio_config_t* radio_cfg) {
    ESP_LOGI(TAG, "=== BOOT STAGE: frequency plan init ===");
    const bramble_freq_plan_t* plan = freq_plan_get_default();
    ESP_LOGI(TAG, "Frequency plan: %s (%.1f MHz, max %d dBm)", plan->name, plan->default_freq_mhz,
             plan->max_tx_power_dbm);

    ESP_LOGI(TAG, "=== BOOT STAGE: radio profile config ===");
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, radio_cfg);

    /* Override with frequency plan values */
    radio_cfg->frequency_mhz = plan->default_freq_mhz;
    radio_cfg->tx_power = freq_plan_clamp_power(plan, radio_cfg->tx_power);
    radio_cfg->sf = plan->default_sf;
    radio_cfg->bw_hz = plan->default_bw_hz;

    /* Check NVS for user-saved radio config (overrides defaults) */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_RADIO, NVS_READONLY, &nvs) == ESP_OK) {
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

    ESP_LOGI(TAG, "Radio config: %.1f MHz SF%d BW%lu TX:%d dBm", radio_cfg->frequency_mhz,
             radio_cfg->sf, (unsigned long)radio_cfg->bw_hz, radio_cfg->tx_power);

    return ESP_OK;
}

/**
 * Perform periodic maintenance: beacons, neighbor purge, route cleanup, etc.
 */

uint32_t s_probe_id;
uint32_t s_probe_sent_ms;
bool s_probe_collecting;
bool s_probe_complete_emitted;
probe_result_t s_probe_results[MAX_PROBE_RESULTS];
int s_probe_result_count;
uint8_t s_probe_rounds_sent;
uint32_t s_probe_next_round_ms;
bool s_probe_request_pending;
uint32_t s_probe_request_id;

static void mesh_periodic_maintenance(uint32_t t, uint32_t* last_beacon_ms,
                                      uint32_t* beacon_interval, uint32_t* last_purge_ms) {
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

    /* Budget floor: stretch the cadence so beacon ToA at the live SF fits
     * its share of the BROADCAST lane (the duty-cycle cap shrinks that
     * lane on EU868). Beacons must fit the budget by design; a cadence the
     * budget cannot fund would otherwise be denied at transmit time and
     * the node would intermittently vanish from neighbor tables. */
    uint32_t budget_floor = tx_gate_beacon_min_interval();
    if (budget_floor > base_interval) {
        static uint32_t s_last_logged_floor = 0;
        if (budget_floor != s_last_logged_floor) {
            ESP_LOGI(TAG,
                     "Beacon interval stretched to %" PRIu32 "ms by airtime budget "
                     "(policy wanted %" PRIu32 "ms)",
                     budget_floor, base_interval);
            s_last_logged_floor = budget_floor;
        }
        base_interval = budget_floor;
    }

    /* Periodic beacon TX */
    if ((t - *last_beacon_ms) >= *beacon_interval) {
        send_beacon();
        *last_beacon_ms = t;

        /* Add jitter for next interval. The span-clamped helper is what keeps
         * a short base (the emulator's EMU_BEACON_INTERVAL_MS override) from
         * summing negative and wrapping the uint32, which permanently stopped
         * beaconing after a handful of beacons. */
        uint8_t j[2];
        crypto_random(j, 2);
        *beacon_interval = beacon_next_interval_ms(base_interval, (uint16_t)(j[0] | (j[1] << 8)));
    }

    /* Periodic identity attestation (Phase 2): low cadence, budget-gated.
     * s_attestation_wait_ms stays 0 until the post-boot send hook arms the
     * schedule, so this never fires before the radio-up boot send. */
    if (s_attestation_wait_ms != 0 && (t - s_attestation_last_ms) >= s_attestation_wait_ms) {
        attempt_identity_attestation(t);
    }

    /* Periodic neighbor purge + route maintenance */
    if ((t - *last_purge_ms) >= NEIGHBOR_PURGE_INTERVAL) {
        neighbor_purge(&s_neighbors, t);
        dedup_purge(&s_dedup, t);
        dedup_purge(&s_flood_dedup, t);
        dedup_purge(&s_delivered_dedup, t);
        route_maintenance(&s_routes, t);
        reverse_route_purge(&s_reverse_routes, t);
        reassembly_purge(&s_reassembly, t);
        *last_purge_ms = t;

        /* Expire old mailbox entries */
        if (s_mailbox_enabled)
            mailbox_expire(t);

        /* Update shared state */
        mesh_publish_neighbors();

        /* Expire queued messages. Route-awaiting entries keep the original
         * flat 60s/log-only behavior (route discovery timing, unrelated to
         * the handshake budget). Session-awaiting entries use DM_QUEUE_TTL_MS
         * (B5: covers the full handshake retransmit budget) and emit
         * onAck status=failed reason="no_secure_session" before freeing, so
         * the UI always sees a clear failure rather than a silent drop. The
         * dm_alloc'd HANDSHAKING slot itself is left alone here: it is
         * reclaimed by dm_alloc's own state-priority LRU eviction the next
         * time a fresh handshake slot is needed, never left occupying an
         * ACTIVE session's protection. */
        for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
            if (!s_queued_msgs[i].used)
                continue;
            uint32_t ttl =
                (s_queued_msgs[i].reason == QUEUE_REASON_SESSION) ? DM_QUEUE_TTL_MS : 60000;
            if ((t - s_queued_msgs[i].timestamp) > ttl) {
                if (s_queued_msgs[i].reason == QUEUE_REASON_SESSION) {
                    ESP_LOGW(TAG, "Queued DM for %08" PRIX32 " expired (no secure session)",
                             s_queued_msgs[i].dest_addr);
                    rerr_fastfail_notify(s_queued_msgs[i].pkt_id, "no_secure_session", NULL);
                } else {
                    ESP_LOGW(TAG, "Queued msg for %08" PRIX32 " expired",
                             s_queued_msgs[i].dest_addr);
                }
                /* The message owns exactly one msg_store row: retire it as
                 * FAILED rather than leaving it pending forever. */
                msg_store_update_by_uid(s_queued_msgs[i].uid, 0, MSG_STATUS_FAILED);
                s_queued_msgs[i].used = false;
            }
        }

        /* Proactive DH-ratchet rekey: bump long-lived / high-volume DM sessions
         * to the next epoch on the purge cadence (Task 4 post-compromise
         * recovery). Cheap table scan, rate-limited per peer. */
        maybe_schedule_dm_epoch_rekey(t);
    }

    /* Issue #72: rate-limited, dirty-gated flush of the replay high-water
     * marks to NVS. Self-throttling (see REPLAY_FLUSH_INTERVAL_MS), so
     * calling it on the maintenance cadence costs a dirty-flag check when
     * there is nothing to write, and never touches flash on the RX path. */
    if (s_replay_flush_requested) {
        s_replay_flush_requested = false;
        mesh_replay_store_save(true);
    } else {
        mesh_replay_store_save(false);
    }

    /* Drain due jittered RREQ forwards every loop iteration (10ms cadence) */
    process_rreq_forward_queue(t);

    /* Drain due jittered channel-flood relays (Task 5) every loop iteration */
    process_flood_relay_queue(t);

    /* Discovery retries (check every 5s) */
    static uint32_t last_disc_check = 0;
    if ((t - last_disc_check) >= 5000) {
        last_disc_check = t;
        for (int i = 0; i < s_pending_disc.count; i++) {
            pending_discovery_t* pd = &s_pending_disc.entries[i];
            if (discovery_should_retry(pd, t)) {
                if (pd->attempts >= MAX_RREQ_ATTEMPTS) {
                    ESP_LOGW(TAG, "Discovery failed for %08" PRIX32 " after %u attempts",
                             pd->dest_addr, pd->attempts);
                    /* Clear queued messages for this dest. Route-awaiting
                     * only: a QUEUE_REASON_SESSION entry's fate is decided
                     * by the handshake, not by route discovery, and has its
                     * own TTL/onAck-failed reaper above. */
                    for (int j = 0; j < MAX_QUEUED_MSGS; j++) {
                        if (s_queued_msgs[j].used &&
                            s_queued_msgs[j].reason == QUEUE_REASON_ROUTE &&
                            s_queued_msgs[j].dest_addr == pd->dest_addr) {
                            msg_store_update_by_uid(s_queued_msgs[j].uid, 0, MSG_STATUS_FAILED);
                            s_queued_msgs[j].used = false;
                        }
                    }
                    discovery_remove(&s_pending_disc, pd->dest_addr);
                    i--; /* re-check same index after remove */
                } else {
                    /* Fresh query_id per retry (DES-2): nodes that heard an
                     * earlier attempt would eat a same-query retry via the
                     * 30s dedup window. A fresh query is a NEW query, so it
                     * gets a fresh pseudonym; incoming RREPs match because
                     * the pending discovery remembers every attempt's
                     * query_id. Retries also widen the ring (DES-1). */
                    uint32_t retry_query = next_packet_id();
                    discovery_record_attempt(pd, retry_query, t);
                    ESP_LOGI(TAG,
                             "Retrying RREQ for %08" PRIX32 " (attempt %u, query=%08" PRIX32
                             ", hop_limit=%u)",
                             pd->dest_addr, pd->attempts, retry_query,
                             discovery_hop_limit_for_attempt(pd->attempts));

                    uint32_t enc_src = rreq_pseudonym_generate(s_identity->private_key,
                                                               s_identity->address, retry_query);

                    bramble_rreq_t rreq = rreq_build_originator(
                        s_identity->address, pd->dest_addr, retry_query, enc_src,
                        discovery_hop_limit_for_attempt(pd->attempts));
                    send_rreq(&rreq);
                }
            }
        }
    }

    /* ACK retry tick: retransmit unacknowledged packets */
    static uint32_t last_ack_tick = 0;
    if ((t - last_ack_tick) >= 1000) { /* Check every 1s */
        last_ack_tick = t;
        for (int i = 0; i < MAX_PENDING_ACKS; i++) {
            pending_ack_t* pa = &s_pending_acks.entries[i];
            if (!pa->active)
                continue;
            if (t >= pa->next_retry_ms) {
                /* Extract packet type from stored packet data for telemetry */
                uint8_t pkt_type = (pa->packet_len >= 2) ? pa->packet_data[1] : 0xFF;

                if (pa->attempt >= pa->max_attempts) {
                    ESP_LOGW(TAG, "Message %08" PRIX32 " to %08" PRIX32 " failed after %u attempts",
                             pa->packet_id, pa->dest_addr, pa->attempt);

                    /* Record timeout event - TX fail represents final timeout */
                    traffic_debug_record_tx(&s_traffic_debug, pkt_type, pa->packet_len, pa->tier);

                    if (msg_store_update_status(pa->packet_id, MSG_STATUS_FAILED)) {
                        /* Same repaint as the delivered path: a give-up is a status
                         * change, and an open thread must stop showing "pending" for
                         * a message that is never going to arrive. */
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
                        ui_graphics_notify(UI_EVT_MSG_STATUS);
#endif
                    }
                    /* Notify webapp of failure */
                    cJSON* params = cJSON_CreateObject();
                    char pkt_buf[12];
                    snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, pa->packet_id);
                    cJSON_AddStringToObject(params, "packet_id", pkt_buf);
                    cJSON_AddStringToObject(params, "status", "failed");
                    rpc_notify("bramble.onAck", params);
                    cJSON_Delete(params);
                    pa->active = false;
                } else {
                    ESP_LOGI(TAG, "Retransmit pkt %08" PRIX32 " to %08" PRIX32 " (attempt %u/%u)",
                             pa->packet_id, pa->dest_addr, pa->attempt + 1, pa->max_attempts);

                    /* Budget-gated retransmission (DES-6). Deny behavior:
                     * a denied retry burns the attempt deliberately. The
                     * original TX already went out once; retries are pure
                     * redundancy, and burning the attempt bounds failure
                     * latency so a saturated mesh yields a visible FAILED
                     * status instead of a zombie pending message. The
                     * exponential backoff below applies either way, so
                     * denial cannot spin the retry tick. */
                    int retry_rc = mesh_tx(pa->packet_data, pa->packet_len, TX_KIND_DATA_RETRY);
                    if (retry_rc == TX_GATE_ERR_BUDGET) {
                        ESP_LOGW(TAG,
                                 "Retry of pkt %08" PRIX32
                                 " denied by airtime budget (attempt %u/%u burned)",
                                 pa->packet_id, pa->attempt + 1, pa->max_attempts);
                    }
                    pa->attempt++;
                    /* Exponential backoff with ±25% jitter (F25) */
                    uint32_t delay = tier_base_delay_ms(pa->tier) << pa->attempt;
                    uint32_t quarter = delay / 4;
                    if (quarter > 0) {
                        delay = delay - quarter + (esp_random() % (2 * quarter + 1));
                    }
                    pa->next_retry_ms = t + delay;
                }
            }
        }
    }

    if (s_probe_collecting && s_probe_rounds_sent < PROBE_SWEEP_ROUNDS &&
        t >= s_probe_next_round_ms) {
        uint8_t round = (uint8_t)(s_probe_rounds_sent + 1);
        mesh_send_probe_round(s_probe_id, round);
        s_probe_rounds_sent = round;
        s_probe_next_round_ms = t + PROBE_SWEEP_INTERVAL_MS;
    }

    /* Probe completion event */
    if (s_probe_collecting && !s_probe_complete_emitted &&
        (t - s_probe_sent_ms) >= PROBE_COLLECTION_WINDOW_MS) {
        cJSON* params = cJSON_CreateObject();
        char pid_buf[12];
        snprintf(pid_buf, sizeof(pid_buf), "%08" PRIX32, s_probe_id);
        cJSON_AddStringToObject(params, "probe_id", pid_buf);
        cJSON_AddNumberToObject(params, "unique_count", s_probe_result_count);
        cJSON_AddNumberToObject(params, "duration_ms", t - s_probe_sent_ms);
        cJSON_AddNumberToObject(params, "rounds_total", PROBE_SWEEP_ROUNDS);

        cJSON* responders = cJSON_AddArrayToObject(params, "responders");
        for (int i = 0; i < s_probe_result_count; i++) {
            probe_result_t* r = &s_probe_results[i];
            int seen_rounds = __builtin_popcount((unsigned)r->seen_round_mask);
            cJSON* item = cJSON_CreateObject();
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

static void mesh_task(void* param) {
    (void)param;

    /* Cast: BaseType_t is int on Xtensa but long on the POSIX port. */
    ESP_LOGI(TAG, "=== BOOT STAGE: mesh_task start (core %d) ===", (int)xPortGetCoreID());

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

    /* Init radio: this is where hangs have been observed on SX1262.
     * The task watchdog will reset the device if radio_init() never returns. */
    ESP_LOGI(TAG, "=== BOOT STAGE: radio_init (SX1262), WDT active ===");
    int ret = radio_init(&radio_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Radio init failed: %d", ret);
        ESP_LOGE(TAG, "Mesh task exiting, no radio");
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.radio_ok = false;
        xSemaphoreGive(s_state_mutex);
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "=== BOOT STAGE: radio initialized, starting RX ===");
    radio_start_rx();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.radio_ok = true;
    xSemaphoreGive(s_state_mutex);

    /* Timing */
    uint32_t last_beacon_ms = 0;
    uint32_t last_purge_ms = 0;
    uint32_t beacon_interval = BEACON_INTERVAL_MS;
    /* Emulator only: EMU_BEACON_INTERVAL_MS shortens beaconing so a short
     * headless scenario discovers neighbors reliably. Apply it to BOTH the
     * initial interval (times the second beacon) and the policy base (which
     * mesh_periodic_maintenance recomputes every subsequent interval from), so
     * beacons stay short throughout, not just once. Guarded on a real override
     * so production and any NVS-loaded policy are untouched. */
    uint32_t emu_beacon_ms = emu_beacon_interval_override_ms();
    if (emu_beacon_ms > 0) {
        beacon_interval = emu_beacon_ms;
        s_beacon_policy.base_interval_ms = emu_beacon_ms;
        if (s_beacon_policy.min_interval_ms > emu_beacon_ms)
            s_beacon_policy.min_interval_ms = emu_beacon_ms;
        /* The override exists to give a short scenario many discovery
         * chances, but at these cadences the beacon budget floor
         * (tx_gate_min_beacon_interval) immediately stretches the interval
         * back to tens of seconds and the bucket denies what the floor
         * does not stop, so discovery collapses to the first two or three
         * beacons. Exempt beacons from the budget for this run; every
         * other kind stays budgeted, and exempt beacons do not debit the
         * lane, so data budgeting in the scenario still matches device
         * behavior. Dead on device builds (the override returns 0 there). */
        tx_gate_set_beacon_budget_exempt(true);
    }

    /* Add initial jitter before first beacon */
    ESP_LOGI(TAG, "=== BOOT STAGE: beacon jitter delay ===");
    uint8_t jitter_buf[2];
    crypto_random(jitter_buf, 2);
    uint32_t initial_delay = (uint32_t)(jitter_buf[0] | (jitter_buf[1] << 8)) % BEACON_JITTER_MS;
    /* Reset WDT during the jitter sleep to avoid spurious WDT triggers */
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(initial_delay));

    /* Fresh WDT reset before send_beacon; TX can block up to 4s waiting for
     * the SX1262 done IRQ.  Without this, jitter_delay + TX can exceed the
     * 5s WDT window and reset the device before the main loop even starts. */
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "=== BOOT STAGE: sending first beacon ===");
    send_beacon();
    last_beacon_ms = now_ms();

    /* Post-boot identity attestation (Phase 2): radio is up and the beacon
     * jitter already spread us out; announce {addr, X25519, Ed25519} once
     * now, then every ATTESTATION_INTERVAL_MS via periodic maintenance. */
    attempt_identity_attestation(now_ms());

    ESP_LOGI(TAG, "=== BOOT STAGE: entering main mesh loop ===");

    /* Main loop */
    while (1) {
        uint32_t t = now_ms();

        /* Reset task watchdog; if this stops being called, WDT resets device */
        esp_task_wdt_reset();

        /* Check if the radio was flagged for reinit and reconfigure if so. The
         * flag is raised either by a stuck-BUSY hard reset or by a soft request
         * after repeated CAD timeouts (issue #118), so the message covers both. */
        if (radio_check_and_clear_reinit()) {
            ESP_LOGW(TAG, "Radio reinit (stuck BUSY or repeated CAD timeout), reconfiguring");
        }

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
                } else if (mesh_evt == MESH_EVT_PROBE_REPLY_TX) {
                    mesh_process_probe_reply_tx_event();
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
uint32_t send_data_packet(uint32_t dest_addr, const uint8_t* payload, size_t payload_len,
                          const bramble_channel_t* ch, uint8_t app_type) {
    uint8_t ciphertext[BRAMBLE_MAX_PACKET_SIZE + CHANNEL_MSG_OVERHEAD + CHANNEL_MSG_SENT_AT_SIZE];
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];
    /* CHAT messages carry an authenticated sent_at inside the ciphertext
     * (Task 0.6), so their wire ciphertext is CHANNEL_MSG_SENT_AT_SIZE bytes
     * longer than other app types; ct_len must account for that or the
     * memcpy/total-size math below truncates the last 4 bytes of a real
     * chat message's ciphertext. sent_at is network time (via timesync,
     * degrading gracefully to local uptime pre-sync) so it is comparable to
     * a receiver's clock in the deferred-acceptance window (handle_data). */
    uint32_t sent_at = (app_type == APP_TYPE_CHAT)
                           ? (uint32_t)(timesync_get_network_time(&s_timesync, now_ms()) / 1000)
                           : 0;
    size_t ct_len = CHANNEL_MSG_OVERHEAD +
                    (app_type == APP_TYPE_CHAT ? CHANNEL_MSG_SENT_AT_SIZE : 0) + payload_len;

    /* Build packet: header(12) + src_addr(4) + prev_hop(4) + nonce(12) +
     * ciphertext(N) + tag(16). Wire v4. */
    size_t total =
        BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + ct_len + BRAMBLE_TAG_SIZE;
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
        /* Reactive: ROUTE_HOP_LIMIT_MAX (must traverse expanded-ring routes).
         * Flood transport: the operator-settable flood hop budget. */
        .hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit),
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };

    bramble_header_serialize(&header, buf, HEADER_SIZE);

    /* AAD excludes hop_limit (relays decrement it in flight) but binds
     * src_addr (SEC-M2 residual); the destination builds the same masked,
     * src-bound AAD in handle_data. */
    uint8_t aad[HEADER_SIZE + 4];
    bramble_build_aead_aad(&header, s_identity->address, aad, sizeof(aad));

    /* Deterministic node-global counter nonce (SEC-C reuse-avoidance): never
     * reuses a nonce under this node's channel keys across its lifetime.
     * Mutex-guarded: send_data_packet is reachable from multiple FreeRTOS
     * tasks (mesh, RPC/httpd, UI, CLI) and the counter has no locking of its
     * own. Abort the send on failure exactly like an encrypt failure: a
     * failed reserve/flush write means nonce_counter_next issued nothing
     * (fail-closed), so there is no nonce to encrypt with. */
    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    xSemaphoreGive(s_nonce_mutex);
    if (nonce_ret != 0) {
        ESP_LOGE(TAG, "Nonce counter unavailable, dropping send: %d", nonce_ret);
        return 0;
    }

    int enc_ret = channel_msg_encrypt(ch, s_identity->address, app_type, sent_at, payload,
                                      payload_len, aad, HEADER_SIZE + 4, nonce, ciphertext, tag);
    if (enc_ret != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed: %d", enc_ret);
        return 0;
    }

    memcpy(buf + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
    /* Wire v4: we are the ORIGINATOR, so prev_hop starts as our own address
     * (the first relay's receiver learns a 1-hop route to us via this).
     * Relay-mutable/MAC-excluded; see packet.h. */
    memcpy(buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    /* Wire v4 (F1): network-key MAC over the origin-stable fields (masked
     * header + src_addr), so relays can gate reverse-route learning without
     * decrypting. Origin-written, carried through every forward unchanged.
     * Mandatory-provisioning (Task 2): abort if unprovisioned (INERT node
     * originates no authenticated DATA). */
    if (data_auth_sign(&header, s_identity->address, buf + BRAMBLE_DATA_AUTH_HMAC_OFFSET) != 0) {
        ESP_LOGD(TAG, "unprovisioned: inert, dropping DATA send");
        return 0;
    }
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext, ct_len);
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + ct_len, tag, BRAMBLE_TAG_SIZE);

    /* Deny behavior: data sends fail visibly upward. A zero return tells
     * every caller (mesh_send_channel, mesh_send_broadcast, RPC) that
     * nothing was transmitted, so the message store and UI never show a
     * phantom send. */
    tx_kind_t kind = (dest_addr == 0xFFFFFFFF) ? TX_KIND_DATA_BROADCAST : TX_KIND_DATA;
    int ret = mesh_tx(buf, (uint8_t)total, kind);
    if (ret == TX_GATE_OK) {
        /* Register for ACK tracking (unicast only). Task 6 (GAP B): tier is
         * decided by msg_tier_for_send, the single source of truth for
         * this -- key exchange (APP_TYPE_KE, the handshake TRANSPORT) must
         * retry at MSG_TIER_CRITICAL (8 attempts), not the MSG_TIER_NORMAL
         * (3 attempts) every other app_type gets, per spec: losing a
         * handshake message stalls session establishment entirely. */
        if (dest_addr != 0xFFFFFFFF) {
            uint8_t tier = msg_tier_for_send(app_type == APP_TYPE_KE);
            pending_ack_add(&s_pending_acks, pkt_id, dest_addr, tier, buf, (uint16_t)total,
                            now_ms());
        }
        return pkt_id;
    }
    return 0;
}

int mesh_send_broadcast(const uint8_t* data, size_t len) {
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
        return -1;
    }
    /* F24: Validate s_channels[0] is actually the public channel */
    if (s_channels[0].channel_id != 0) {
        ESP_LOGE(TAG, "mesh_send_broadcast: s_channels[0] is not the public channel (id=%u)",
                 (unsigned)s_channels[0].channel_id);
        return -1;
    }
    ESP_LOGI(TAG, "mesh_send_broadcast using idx0 channel_id=%u",
             (unsigned)s_channels[0].channel_id);

    /* Pre-check with the real ToA of the first packet that would hit the
     * air, so RPC callers get a clean -2 before paying for encryption.
     * The gate still checks and debits every individual transmission. */
    size_t est_payload = (len > FRAG_MAX_PLAINTEXT) ? FRAG_MAX_PLAINTEXT : len;
    size_t est_wire = BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE +
                      CHANNEL_MSG_OVERHEAD + est_payload + BRAMBLE_TAG_SIZE;
    if (est_wire > 255)
        est_wire = 255;
    tx_gate_set_peer_count((uint8_t)neighbor_count(&s_neighbors));
    if (!tx_gate_check((uint8_t)est_wire, TX_KIND_DATA_BROADCAST)) {
        ESP_LOGW(TAG, "Broadcast rate limited by airtime budget (remaining=%" PRIu32 "ms)",
                 tx_gate_remaining(AIRTIME_TIER_BROADCAST));
        return -2;
    }

    /* Check if fragmentation is needed */
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Long message: split into fragments */
        uint16_t msg_id = (uint16_t)(next_packet_id() & 0xFFFF);
        fragment_t* frags = calloc(FRAG_MAX_FRAGMENTS, sizeof(fragment_t));
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

        ESP_LOGI(TAG, "Sending broadcast message as %d fragments (msg_id=%04X)", num_frags, msg_id);

        /* A receipt for a fragmented broadcast correlates on the FIRST fragment's
         * packet_id (that is the id recorded in s_last_broadcast_id), so the store
         * row has to be filed under that same id to be reachable. */
        uint32_t first_pkt_id = 0;

        /* Send each fragment with pacing */
        for (int i = 0; i < num_frags; i++) {
            uint32_t pkt_id =
                send_data_packet(0xFFFFFFFF, frags[i].data, frags[i].len, &s_channels[0], 0x01);
            if (i == 0 && pkt_id != 0) {
                s_last_broadcast_id = pkt_id;
                first_pkt_id = pkt_id;
            }
            if (pkt_id == 0) {
                ESP_LOGW(TAG, "Fragment %d transmission failed", i);
            } else {
                recent_broadcast_record(pkt_id);
                ESP_LOGI(TAG, "Sent fragment %d/%d (pkt_id=%08" PRIX32 ")", i + 1, num_frags,
                         pkt_id);
            }

            /* Inter-fragment pacing to avoid flooding */
            if (i < num_frags - 1) {
                vTaskDelay(pdMS_TO_TICKS(50)); /* 50ms between fragments */
            }
        }

        free(frags);

        /* Store the full message in message store (broadcast = channel 0) */
        msg_store_add_channel(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, (const char*)data, len, 0, 0,
                              first_pkt_id, MSG_STATUS_NONE, 0);
        return 0;
    }

    /* Short message: fast path (no fragmentation) */
    uint32_t pkt_id = send_data_packet(0xFFFFFFFF, data, len, &s_channels[0], 0x01);
    if (pkt_id != 0) {
        s_last_broadcast_id = pkt_id;
        recent_broadcast_record(pkt_id);
        /* File the row under the packet_id we just transmitted, not 0. A row with
         * packet_id 0 is untrackable by construction: a delivery receipt is matched
         * against the store by the orig_packet_id the receiver echoes back, so a
         * zero-id row can never be promoted to DELIVERED and the message stays
         * unconfirmed forever. Status stays NONE here (a broadcast is not "sent to"
         * anyone in particular) until a receipt arrives, matching the sibling path
         * in mesh_send_channel. */
        msg_store_add_channel(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, (const char*)data, len, 0, 0,
                              pkt_id, MSG_STATUS_NONE, 0);
    }
    return pkt_id ? 0 : -1;
}

/* uid: see mesh_send_message_uid. Only the unicast (DM) path consumes it;
 * broadcasts have no multi-stage pipeline whose rows could duplicate. */
static uint32_t mesh_send_channel_uid(int channel_idx, uint32_t dest_addr, const uint8_t* data,
                                      size_t len, uint32_t uid) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGE(TAG, "Invalid channel index: %d (count=%d)", channel_idx, s_num_channels);
        msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        return 0;
    }

    /* SEC-C2: every unicast send (whether via mesh_send_message's default
     * channel or an explicit channel picked by rpc_methods.c) converges
     * here before any bytes are encrypted. Broadcasts (dest_addr ==
     * 0xFFFFFFFF) have no peer to hold a session with and always fall
     * through to the channel-key path below unchanged. */
    if (dest_addr != 0xFFFFFFFFu) {
        return mesh_send_dm(channel_idx, dest_addr, data, len, uid);
    }

    /* Check if fragmentation is needed */
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Long message: split into fragments */
        uint16_t msg_id = (uint16_t)(next_packet_id() & 0xFFFF);
        fragment_t* frags = calloc(FRAG_MAX_FRAGMENTS, sizeof(fragment_t));
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

        ESP_LOGI(TAG, "Sending channel message as %d fragments (msg_id=%04X)", num_frags, msg_id);

        uint32_t first_pkt_id = 0;
        /* Send each fragment with pacing */
        for (int i = 0; i < num_frags; i++) {
            uint32_t pkt_id = send_data_packet(dest_addr, frags[i].data, frags[i].len,
                                               &s_channels[channel_idx], 0x01);
            if (pkt_id == 0) {
                ESP_LOGW(TAG, "Fragment %d transmission failed", i);
            } else {
                if (i == 0) {
                    first_pkt_id = pkt_id;
                    if (dest_addr == 0xFFFFFFFFu) {
                        s_last_broadcast_id = pkt_id;
                    }
                }
                if (dest_addr == 0xFFFFFFFFu) {
                    recent_broadcast_record(pkt_id);
                }
                ESP_LOGI(TAG, "Sent fragment %d/%d (pkt_id=%08" PRIX32 ")", i + 1, num_frags,
                         pkt_id);
            }

            /* Inter-fragment pacing to avoid flooding */
            if (i < num_frags - 1) {
                vTaskDelay(pdMS_TO_TICKS(50)); /* 50ms between fragments */
            }
        }

        free(frags);

        /* Store the full message in message store */
        if (first_pkt_id != 0) {
            msg_store_add_channel(
                dest_addr,
                (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_DIR_BROADCAST_OUT
                                                              : MSG_DIR_OUTGOING,
                (const char*)data, len, 0, 0, first_pkt_id,
                (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_STATUS_NONE : MSG_STATUS_SENT,
                (uint8_t)channel_idx);
        }
        return first_pkt_id;
    }

    /* Short message: fast path (no fragmentation) */
    uint32_t pkt_id = send_data_packet(dest_addr, data, len, &s_channels[channel_idx], 0x01);
    if (pkt_id != 0) {
        if (dest_addr == 0xFFFFFFFFu) {
            s_last_broadcast_id = pkt_id;
            recent_broadcast_record(pkt_id);
        }
        msg_store_add_channel(dest_addr,
                              (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_DIR_BROADCAST_OUT
                                                                            : MSG_DIR_OUTGOING,
                              (const char*)data, len, 0, 0, pkt_id,
                              (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_STATUS_NONE
                                                                            : MSG_STATUS_SENT,
                              (uint8_t)channel_idx);
    }
    return pkt_id;
}

uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t* data, size_t len) {
    return mesh_send_channel_uid(channel_idx, dest_addr, data, len, 0);
}

static int queue_message(uint32_t dest_addr, const uint8_t* data, size_t len, uint32_t uid) {
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used) {
            s_queued_msgs[i].dest_addr = dest_addr;
            memcpy(s_queued_msgs[i].data, data, len);
            s_queued_msgs[i].len = len;
            s_queued_msgs[i].timestamp = now_ms();
            s_queued_msgs[i].reason = QUEUE_REASON_ROUTE;
            s_queued_msgs[i].pkt_id = 0;
            s_queued_msgs[i].uid = uid;
            s_queued_msgs[i].channel_idx = 0;
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
     * This provides unlinkability: every attempt (first try or retry) is a
     * new query with a fresh query_id acting as the nonce, so observers
     * cannot correlate route requests from the same originator.
     *
     * The pseudonym is keyed with the originator's PRIVATE key, so nobody
     * (including the destination) can reverse-map it; the originator
     * identifies itself during the secure channel setup phase instead.
     *
     * Nothing stores the pseudonym: RREPs are correlated by query_id via the
     * pending-discovery table, which remembers every attempt's query_id. */
    uint32_t pseudonym =
        rreq_pseudonym_generate(s_identity->private_key, s_identity->address, query_id);

    ESP_LOGI(TAG,
             "RREQ privacy: addr=%08" PRIX32 " → pseudonym=%08" PRIX32 " (query=%08" PRIX32 ")",
             s_identity->address, pseudonym, query_id);

    uint32_t encrypted_source = pseudonym;
    bramble_rreq_t rreq =
        rreq_build_originator(s_identity->address, dest_addr, query_id, encrypted_source,
                              discovery_hop_limit_for_attempt(1));
    send_rreq(&rreq);
    return 0;
}

static uint32_t mesh_send_message_uid(uint32_t dest_addr, const uint8_t* data, size_t len,
                                      uint32_t uid) {
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
        msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        return 0;
    }

    /* Flooding F1 Task 3: send-side flood origination. Under s_flood_transport
     * there is NO route discovery: a unicast message FLOODS immediately, like a
     * broadcast. Gate the whole reactive neighbor/route + RREQ/queue block on
     * the toggle so that under flood we fall straight through to
     * mesh_send_channel, which builds the DATA (dest = D, hop_limit =
     * ROUTE_HOP_LIMIT_MAX, the flood hop budget that broadcast floods already
     * originate at; network-key auth-signed, AEAD-encrypted under the DM
     * session key for D) and hands it to mesh_tx as one budget-gated
     * transmission. Every relay then floods it (Task 1); D's flooded ACK
     * (Task 2) confirms it. send_data_packet/send_dm_packet register the
     * pending-confirmation whose pending_ack_tick retry re-transmits the SAME
     * stored broadcast frame (same packet_id) on no-ACK, which IS a re-flood
     * (mesh_tx never route-looks-up: relays flood it, and the destination's
     * Phase 1 s_delivered_dedup re-ACK-on-duplicate gives another confirmation
     * chance), bounded by the tier max retries then FAILED. If no DM session to
     * D exists yet, mesh_send_dm still kicks off the KE handshake, whose
     * APP_TYPE_KE envelope rides this same DATA flood path (send_ke_envelope ->
     * send_data_packet), so key establishment floods too and stays CRITICAL
     * tier. Toggle OFF (default): the reactive discovery+queue path is exactly
     * as before this task. */
    /* For non-neighbor destinations, check route table */
    neighbor_entry_t* nb = s_flood_transport ? NULL : neighbor_lookup(&s_neighbors, dest_addr);
    if (!s_flood_transport && !nb) {
        /* Not a direct neighbor, need routing */
        route_entry_t* route = route_lookup(&s_routes, dest_addr);
        if (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING) {
            /* No route: start discovery and queue the message */
            if (!discovery_lookup(&s_pending_disc, dest_addr)) {
                initiate_discovery(dest_addr);
            }
            /* One row per user message: store it pending HERE (uid == 0, the
             * first stage to see it) and carry that uid on the queue entry, so
             * flush_queued_messages, mesh_send_dm and flush_session_queue all
             * UPDATE this row instead of each adding their own copy. A unicast
             * DM is stored channel-less. */
            uint32_t row_uid = (uid != 0) ? uid : msg_store_next_uid();
            if (queue_message(dest_addr, data, len, row_uid) != 0) {
                msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
                return 0;
            }
            if (uid == 0) {
                msg_store_add_dm_uid(dest_addr, MSG_DIR_OUTGOING, (const char*)data, len, 0, 0, 0,
                                     MSG_STATUS_NONE, row_uid);
            }
            return 1; /* queued, nonzero = success but no packet_id yet */
        }
        /* Have a route: send_data_packet will transmit (next hop gets it) */
    }

    int send_idx = s_default_channel_idx;
    if (send_idx < 0 || send_idx >= s_num_channels) {
        send_idx = 0;
    }
    return mesh_send_channel_uid(send_idx, dest_addr, data, len, uid);
}

uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len) {
    return mesh_send_message_uid(dest_addr, data, len, 0);
}

uint32_t mesh_resend_message(uint32_t dest_addr, const uint8_t* data, size_t len, uint32_t uid) {
    return mesh_send_message_uid(dest_addr, data, len, uid);
}

bool mesh_park_message(uint32_t uid) {
    if (uid == 0)
        return false;
    if (!msg_store_update_by_uid(uid, 0, MSG_STATUS_QUEUED))
        return false;

    /* Arm the peer so its next beacon re-sends this, for the case the rejoin
     * edge structurally cannot reach: a peer that is present but not ACKing
     * keeps beaconing, so it never leaves the neighbor table and can never be
     * newly admitted to it. A peer that is NOT in the table is left alone;
     * arriving is what the rejoin edge already watches for.
     *
     * Both store calls above and below release MSG_LOCK before this takes
     * s_state_mutex: the two locks are separate and non-recursive, and nesting
     * them in one order here and the other elsewhere is a deadlock. This runs
     * on the UI/RPC caller's task, not the mesh task, so the write into
     * s_neighbors is a cross-task one and takes the mutex the same way
     * mesh_get_peer_name's cross-task read does. */
    uint32_t peer_addr = 0;
    if (!msg_store_peer_for_uid(uid, &peer_addr))
        return true; /* parked; the row was evicted before we could read its peer */
    uint32_t t = now_ms();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    parked_retry_arm(&s_neighbors, peer_addr, t);
    xSemaphoreGive(s_state_mutex);
    return true;
}

bool mesh_cancel_parked_message(uint32_t uid) {
    /* msg_store_update_by_uid refuses QUEUED -> FAILED (parked is sticky, so
     * a failed send retry can't silently un-park a row); cancel needs the
     * one deliberate door out of QUEUED that msg_store_unpark provides. */
    return msg_store_unpark(uid);
}

int mesh_flush_parked_for(uint32_t peer_addr) {
    /* static: mesh_flush_parked_for runs only on the mesh task, never
     * reentrantly, so these are safe off the stack. uids alone is up to
     * MSG_STORE_MAX * sizeof(uint32_t) (800 bytes at the 200-row tdeck-plus
     * cap), which is worth keeping off the packet RX call path's stack. */
    static uint32_t uids[MSG_STORE_MAX];
    static stored_msg_t msg;
    int n = msg_store_parked_uids_for_peer(peer_addr, uids, MSG_STORE_MAX);
    if (n <= 0)
        return 0;

    ESP_LOGI(TAG, "Flushing %d parked message(s) for %08" PRIX32, n, peer_addr);
    for (int i = 0; i < n; i++) {
        if (!msg_store_get_copy_by_uid(uids[i], &msg)) {
            /* Expected, not an error: the row can be evicted from the ring
             * between msg_store_parked_uids_for_peer's selection above and
             * this lookup, on a small or busy ring. Skip this uid and keep
             * going with the rest of the batch. */
            continue;
        }
        /* A failed send leaves the row parked, not FAILED: msg_store's
         * QUEUED -> FAILED transition is sticky-refused (msg_store.h,
         * msg_store_update_by_uid), so nothing in the resend pipeline below
         * can un-park this row just because this attempt failed. It waits
         * for the next genuine rejoin rather than retrying against a peer
         * that is present but unreachable. */
        uint32_t pkt =
            mesh_resend_message(msg.peer_addr, (const uint8_t*)msg.text, msg.text_len, msg.uid);
        ESP_LOGI(TAG, "Parked uid=%" PRIu32 " -> pkt=%08" PRIX32, msg.uid, pkt);
    }
    return n;
}

#ifdef CONFIG_IDF_TARGET_LINUX
/* Emulator only: the address of this node's first known neighbor, or 0 if it has
 * none yet. A scenario's scripted sender (emu_autosend.c) uses it to DM a peer
 * whose self-minted address it cannot know ahead of time; it learns the peer
 * from beacons exactly as the real UI would before composing a direct message. */
uint32_t emu_mesh_first_neighbor(void) {
    mesh_shared_state_t st;
    mesh_get_state(&st);
    if (st.neighbors.count > 0)
        return st.neighbors.entries[0].addr;
    return 0;
}
#endif

/* ── Public API ──────────────────────────────────────────────────────── */

void mesh_task_start(bramble_identity_t* identity) {
    s_identity = identity;

    /* Load node name from NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_node_name);
        esp_err_t name_err = nvs_get_str(nvs, "node_name", s_node_name, &len);
        if (name_err != ESP_OK) {
            if (name_err == ESP_ERR_NVS_INVALID_LENGTH) {
                /* Stored string exceeds buffer, read truncated and force null-terminate */
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
                ESP_LOGW(TAG, "NVS node_name length %u exceeds buffer, truncated", (unsigned)len);
            }
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Node name: %s", s_node_name[0] ? s_node_name : "(none)");

    neighbor_init(&s_neighbors);

    /* Deterministic node-global AEAD nonce counter (SEC-C reuse-avoidance).
     * boot_salt is a secondary defense if NVS is ever wiped. Created before
     * any task that can reach send_data_packet (RPC/httpd, UI, CLI) starts
     * running, so nonce_counter_next is never called without the mutex. */
    s_nonce_mutex = xSemaphoreCreateMutex();
    if (nonce_counter_init(s_identity->address, (uint16_t)(esp_random() & 0xFFFF), mesh_nonce_read,
                           mesh_nonce_write, NULL) != 0) {
        /* Fail-closed by design: nonce_counter_next also refuses to issue
         * until a write succeeds, so encrypted DATA sends stay blocked
         * (never silently reuse a nonce) rather than crashing here. */
        ESP_LOGE(
            TAG,
            "Nonce counter reserve-ahead write failed; encrypted sends blocked until NVS recovers");
    }

    /* DM session table (SEC-C2). Same "created before any task that can
     * reach the guarded state starts" rule as s_nonce_mutex above. */
    s_dm_mutex = xSemaphoreCreateMutex();
    /* Try PSRAM first (keeps ~40KB off scarce internal DRAM), fall back to
     * internal RAM on a board without PSRAM. Same pattern as s_delivery_event_ring
     * below; assert() is unsafe here since it compiles out in release builds. */
    s_dm_table = heap_caps_calloc(1, sizeof(dm_table_t), MALLOC_CAP_SPIRAM);
    if (!s_dm_table) {
        ESP_LOGW(TAG, "No PSRAM for DM session table, using internal RAM (%u bytes)",
                 (unsigned)sizeof(dm_table_t));
        s_dm_table = calloc(1, sizeof(dm_table_t));
    }
    if (!s_dm_table) {
        ESP_LOGE(TAG, "Failed to allocate DM session table (%u bytes)",
                 (unsigned)sizeof(dm_table_t));
        return;
    }
    dm_table_init(s_dm_table);
    memset(s_hs_dedup, 0, sizeof(s_hs_dedup));
    memset(s_pending_eph, 0, sizeof(s_pending_eph));
    s_handshake_work_q = xQueueCreate(HANDSHAKE_WORK_QUEUE_LEN, sizeof(dm_handshake_work_item_t));
    if (!s_handshake_work_q) {
        ESP_LOGE(TAG, "Failed to create handshake work queue");
    }
    /* M7 offload: low priority so the occasional handshake never preempts
     * the mesh RX task; small stack, the only work here is periodic X25519. */
    xTaskCreate(handshake_worker_task, "dm_hs_worker", DM_HANDSHAKE_WORKER_STACK, NULL,
                DM_HANDSHAKE_WORKER_PRIORITY, NULL);

    /* Loads a provisioned network key from NVS if one has been set. If none
     * has, the node stays UNPROVISIONED and inert: network_key_get() fails
     * closed (no PSK fallback), so nothing authenticated is sent or accepted
     * until a real per-fleet key is provisioned. */
    mesh_load_network_key();
    mesh_rederive_beacon_key();

    dedup_init(&s_dedup);
    dedup_init(&s_flood_dedup);
    dedup_init(&s_delivered_dedup);
    replay_table_init(&s_replay);
    replay_table_init(&s_control_replay);
    /* Issue #72: restore the persisted per-sender high-water marks so a
     * reboot (notably an OTA reboot) does not reopen the replay window. */
    mesh_replay_store_load();
    identity_store_init(&s_identity_pins, now_ms());
    /* Trust-anchor campaign (P2): if the fleet anchor was provisioned (loaded
     * from NVS in main.c before this task starts), mark the pin store ANCHORED
     * so it pins ONLY anchor-endorsed identities. Absent = not anchored = the
     * default TOFU behavior, untouched. setAnchor at runtime refreshes this via
     * mesh_set_pin_anchor without a reboot. */
    if (identity_anchor_is_set()) {
        uint8_t anchor_pub[BRAMBLE_ED25519_PUBKEY_SIZE];
        if (identity_anchor_get(anchor_pub) == 0) {
            identity_store_set_anchor(&s_identity_pins, anchor_pub);
        }
    }
    /* Restore the persisted verified TOFU pin table (DM forward-secrecy + SAS)
     * AFTER the anchor is provisioned above: identity_store_deserialize rebuilds
     * the pin bindings + verified bits while preserving the anchor, so a
     * "verified once, stays verified" model survives reboot. Must follow
     * set_anchor (which drops pins on a change) so restored pins are not wiped. */
    mesh_pin_store_load();
    replay_deferred_init(&s_deferred);
    rreq_rate_init(&s_rreq_rl);
    rreq_fwd_init(&s_rreq_fwd_rl, now_ms());
    route_init(&s_routes);
    rreq_dedup_init(&s_rreq_dedup);
    reverse_route_init(&s_reverse_routes);
    discovery_init(&s_pending_disc);
    pending_ack_init(&s_pending_acks);
    {
        /* One TX path: the gate owns the airtime budget and applies the
         * regulatory duty-cycle cap from the frequency plan (DES-8). */
        const bramble_freq_plan_t* plan = freq_plan_get_default();
        tx_gate_global_init(plan->max_duty_cycle_pct, plan->duty_cycle_enforced);
    }
    traffic_debug_init(&s_traffic_debug, s_traffic_events, TRAFFIC_DEBUG_CAPACITY);
    timesync_init(&s_timesync);
    mesh_traffic_debug_load_config(); /* Restore persisted debug config */
    traffic_debug_set_notify_callback(&s_traffic_debug, traffic_event_notify, NULL);
    mesh_beacon_policy_load_config(); /* Restore persisted beacon policy config */
    reassembly_init(&s_reassembly);
    location_init(&s_location_mgr);
    memset(s_queued_msgs, 0, sizeof(s_queued_msgs));
    memset(s_rreq_fwd_queue, 0, sizeof(s_rreq_fwd_queue)); /* Init jittered RREQ forward queue */
    memset(s_flood_relay_queue, 0,
           sizeof(s_flood_relay_queue)); /* Init jittered channel-flood relay queue (Task 5) */
    s_flood_relay_drops = 0;
    probe_ingress_init(&s_probe_ingress, now_ms()); /* PROBE ingress buckets (issue #75) */
    memset(&s_shared, 0, sizeof(s_shared));
    tx_gate_snapshot(&s_shared.airtime);

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

        if (channel_storage_load(loaded_channels, &loaded_count, loaded_names, &loaded_default) ==
                0 &&
            loaded_count > 0) {
            /* Merge loaded channels, preserving channel 0 (public) which is already initialized */
            for (int i = 0; i < loaded_count && s_num_channels < MAX_CHANNELS; i++) {
                /* Skip channel 0 if it was saved (public channel is always first) */
                if (loaded_channels[i].channel_id == 0 && i == 0) {
                    /* Copy the name if it exists */
                    if (loaded_names[i][0] != '\0') {
                        strncpy(s_channel_names[0], loaded_names[i],
                                sizeof(s_channel_names[0]) - 1);
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
                    s_channel_names[s_num_channels][sizeof(s_channel_names[s_num_channels]) - 1] =
                        '\0';
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
            ESP_LOGI(TAG, "Total channels after NVS load: %d (default=%d)", s_num_channels,
                     s_default_channel_idx);
        }
    }

    /* Initialize component mailbox and load enabled state from NVS */
    {
        mailbox_init(&s_mailbox);
        s_mailbox.enabled = true; /* component always ready; runtime gate is s_mailbox_enabled */

        nvs_handle_t mb_nvs;
        if (nvs_open(NVS_NS_MAILBOX, NVS_READONLY, &mb_nvs) == ESP_OK) {
            uint8_t enabled = 0;
            if (nvs_get_u8(mb_nvs, "enabled", &enabled) == ESP_OK) {
                s_mailbox_enabled = (enabled != 0);
                ESP_LOGI(TAG, "Mailbox: %s (from NVS)", s_mailbox_enabled ? "enabled" : "disabled");
            }
            nvs_close(mb_nvs);
        }
        ESP_LOGI(TAG,
                 "Mailbox component initialized: max_entries=%d per_dest=%d per_src=%d ttl=24h",
                 MAILBOX_MAX_ENTRIES, MAILBOX_MAX_PER_DEST, MAILBOX_MAX_PER_SOURCE);
    }

    /* Flooding F1 Task 1: load the flood-transport toggle from NVS. */
    {
        nvs_handle_t fl_nvs;
        if (nvs_open(NVS_NS_FLOOD, NVS_READONLY, &fl_nvs) == ESP_OK) {
            uint8_t enabled = 0;
            if (nvs_get_u8(fl_nvs, "enabled", &enabled) == ESP_OK) {
                s_flood_transport = (enabled != 0);
                ESP_LOGI(TAG, "Flood transport: %s (from NVS)",
                         s_flood_transport ? "enabled" : "disabled");
            }
            /* Flooding F1 finalize: operator-settable flood hop budget. Clamp
             * on load so a stale/out-of-range NVS value can never originate an
             * invalid hop_limit. */
            uint8_t hop_limit = 0;
            if (nvs_get_u8(fl_nvs, "hop_limit", &hop_limit) == ESP_OK) {
                s_flood_hop_limit = flood_hop_limit_clamp(hop_limit);
                ESP_LOGI(TAG, "Flood hop limit: %u (from NVS)", (unsigned)s_flood_hop_limit);
            }
            nvs_close(fl_nvs);
        }
    }

    s_state_mutex = xSemaphoreCreateMutex();
    s_delivery_event_mutex = xSemaphoreCreateMutex();

    /* Refill the location cache that location_init() zeroed above from the
     * peer positions already on flash, so the map knows where its peers are
     * without waiting for a fresh share. Deliberately here and not beside
     * location_init: the apply phase takes s_state_mutex, which does not exist
     * until the line above. */
    mesh_peer_location_restore();
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
        ESP_LOGE(TAG, "Failed to create mesh queues (rx=%p evt=%p)", (void*)s_rx_queue,
                 (void*)s_mesh_event_queue);
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

    memset(s_probe_reply_queue, 0, sizeof(s_probe_reply_queue));
    s_probe_reply_timer = NULL;
    esp_timer_create_args_t probe_reply_timer_args = {
        .callback = mesh_probe_reply_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "probe_reply_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t probe_reply_timer_err =
        esp_timer_create(&probe_reply_timer_args, &s_probe_reply_timer);
    if (probe_reply_timer_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create probe reply timer: %d", (int)probe_reply_timer_err);
        return;
    }

    /* Pin to CPU1: leave CPU0 for UI/display */
#ifdef CONFIG_IDF_TARGET_LINUX
    /* The POSIX simulation has a single core; pinning to CPU1 trips the
     * kernel's core-count assert. */
    xTaskCreatePinnedToCore(mesh_task, "mesh", MESH_TASK_STACK, NULL, MESH_TASK_PRIORITY, NULL,
                            tskNO_AFFINITY);
    ESP_LOGI(TAG, "Mesh task created (no core affinity: single-core simulator)");
#else
    xTaskCreatePinnedToCore(mesh_task, "mesh", MESH_TASK_STACK, NULL, MESH_TASK_PRIORITY, NULL, 1);
    ESP_LOGI(TAG, "Mesh task created (pinned to CPU1)");
#endif
}

void mesh_get_state(mesh_shared_state_t* out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_shared;
    xSemaphoreGive(s_state_mutex);
}

bool mesh_route_is_usable(uint32_t dest_addr) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const route_entry_t* r = route_lookup(&s_routes, dest_addr);
    bool usable = (r != NULL && r->state != ROUTE_BROKEN && r->state != ROUTE_STALE);
    xSemaphoreGive(s_state_mutex);
    return usable;
}

bool mesh_get_neighbor(uint32_t addr, neighbor_entry_t* out) {
    if (!out)
        return false;
    bool found = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (int i = 0; i < s_shared.neighbors.count && i < MAX_NEIGHBORS; i++) {
        if (s_shared.neighbors.entries[i].addr == addr) {
            *out = s_shared.neighbors.entries[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return found;
}

void mesh_get_routes(routing_table_t* out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_routes;
    xSemaphoreGive(s_state_mutex);
}

void mesh_set_node_name(const char* name) {
    if (name && strlen(name) < sizeof(s_node_name)) {
        strncpy(s_node_name, name, sizeof(s_node_name) - 1);
        s_node_name[sizeof(s_node_name) - 1] = '\0';
    } else {
        s_node_name[0] = '\0';
    }
    ESP_LOGI(TAG, "Node name updated: %s", s_node_name[0] ? s_node_name : "(none)");
}

int mesh_set_node_name_persist(const char* name) {
    if (!name || name[0] == '\0' || strlen(name) >= sizeof(s_node_name)) {
        return -1;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs) != ESP_OK) {
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

void mesh_set_flood_transport(bool enabled) {
    s_flood_transport = enabled;
    ESP_LOGI(TAG, "Flood transport runtime: %s", enabled ? "enabled" : "disabled");
}

bool mesh_get_flood_transport(void) { return s_flood_transport; }

void mesh_set_flood_hop_limit(uint32_t hops) { s_flood_hop_limit = flood_hop_limit_clamp(hops); }

uint8_t mesh_get_flood_hop_limit(void) { return s_flood_hop_limit; }

const char* mesh_get_node_name(void) {
    if (s_node_name[0] == '\0')
        return NULL;
    return s_node_name;
}

int mesh_get_identity(uint32_t* addr_out, uint8_t pubkey_out[32]) {
    if (!s_identity || !addr_out || !pubkey_out)
        return -1;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *addr_out = s_identity->address;
    memcpy(pubkey_out, s_identity->public_key, 32);
    xSemaphoreGive(s_state_mutex);
    return 0;
}

void mesh_get_identity_pin_stats(uint32_t* pins, uint32_t* conflicts, uint32_t* sig_failures,
                                 uint32_t* addr_mismatches, uint32_t* unendorsed,
                                 uint32_t* expired) {
    /* Diagnostics-only reads of word-sized counters mutated exclusively on
     * the mesh task (handle_identity_attestation); a momentarily stale
     * value is fine for getStatus, so no mutex, matching the other
     * counter-style getters. */
    if (pins)
        *pins = (uint32_t)identity_store_count(&s_identity_pins);
    if (conflicts)
        *conflicts = s_identity_pins.conflicts;
    if (sig_failures)
        *sig_failures = s_identity_pins.sig_failures;
    if (addr_mismatches)
        *addr_mismatches = s_identity_pins.addr_mismatches;
    if (unendorsed)
        *unendorsed = s_identity_pins.unendorsed;
    if (expired)
        *expired = s_identity_pins.expired;
}

/* Snapshots this node's identity, looks up the peer's pin, and derives the
 * identity SAS. Returns the pin (or NULL if unpinned); sas_out valid only
 * when it returns non-NULL and derivation succeeded (*ok). Shared by
 * mesh_get_peer_verification and mesh_set_peer_verified so both derive the
 * identity SAS the same way, via the same mesh_get_identity snapshot. */
static const identity_pin_t* derive_peer_sas(uint32_t addr, char sas_out[8], bool* ok) {
    *ok = false;
    uint32_t self_addr;
    uint8_t self_pub[32];
    if (mesh_get_identity(&self_addr, self_pub) != 0)
        return NULL;

    /* Lock-free read of s_identity_pins, matching mesh_get_identity_pin_stats:
     * the mesh task is the only mutator, and a momentarily stale snapshot is
     * fine for this UX surface. */
    const identity_pin_t* pinned = identity_store_lookup(&s_identity_pins, addr);
    if (!pinned)
        return NULL;

    *ok = dm_derive_identity_sas(self_pub, pinned->x25519_pub, self_addr, addr, sas_out) == 0;
    return pinned;
}

bool mesh_get_peer_verification(uint32_t addr, char sas_out[8], bool* verified, bool* key_changed) {
    if (!s_identity || !sas_out || !verified || !key_changed)
        return false;

    bool ok;
    const identity_pin_t* pinned = derive_peer_sas(addr, sas_out, &ok);
    if (!pinned || !ok)
        return false;

    *verified = pinned->verified;
    *key_changed = pinned->key_changed;
    return true;
}

bool mesh_get_peer_verify_flags(uint32_t addr, bool* verified, bool* key_changed) {
    if (!verified || !key_changed)
        return false;

    /* Lock-free read of s_identity_pins, matching mesh_get_peer_verification:
     * the mesh task is the only mutator, and a momentarily stale snapshot is
     * fine for this UX surface. No dm_derive_identity_sas here (no SAS is
     * consumed by callers of this accessor). */
    const identity_pin_t* pinned = identity_store_lookup(&s_identity_pins, addr);
    if (!pinned) {
        *verified = false;
        *key_changed = false;
        return false;
    }

    *verified = pinned->verified;
    *key_changed = pinned->key_changed;
    return true;
}

bool mesh_set_peer_verified(uint32_t addr, bool verified) {
    if (!s_identity)
        return false;

    if (verified) {
        /* Derive the same identity SAS as mesh_get_peer_verification to
         * record with the pin. */
        char sas[8];
        bool ok;
        const identity_pin_t* pinned = derive_peer_sas(addr, sas, &ok);
        if (!pinned || !ok)
            return false;

        /* set_verified also clears key_changed: re-verifying dismisses the
         * warning (it is NOT the genuine key-change site). */
        identity_store_set_verified(&s_identity_pins, addr, sas);
    } else {
        /* A deliberate user un-verify is not a key change; do not call
         * identity_store_mark_key_changed here. Still requires a pin to
         * exist (matches the prior behavior: unpinned addr -> false). */
        const identity_pin_t* pinned = identity_store_lookup(&s_identity_pins, addr);
        if (!pinned)
            return false;
        identity_store_clear_verified(&s_identity_pins, addr);
    }
    mesh_pin_store_save();
    return true;
}

/* Trust-anchor campaign (P2): refresh the live pin store's anchor after a
 * runtime setAnchor, so provisioning an anchor takes effect on the pin gate
 * without a reboot. Called from the RPC path (rpc_set_anchor). The pin store
 * is otherwise mutated only on the mesh task; a setAnchor write here races an
 * in-flight attestation read benignly (has_anchor only ever goes false->true,
 * and the 32-byte key copy resolves within one attestation cadence), matching
 * the lock-free convention mesh_trigger_attestation already uses cross-thread.
 * The boot path (mesh_task_start) also loads the anchor, so a reboot is never
 * required for correctness; this just avoids waiting for one. */
void mesh_set_pin_anchor(const uint8_t anchor_pub[BRAMBLE_ED25519_PUBKEY_SIZE]) {
    identity_store_set_anchor(&s_identity_pins, anchor_pub);
}

const char* mesh_get_peer_name(uint32_t addr) {
    static char s_name_buf[17];

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    neighbor_entry_t* nb = neighbor_lookup(&s_neighbors, addr);
    if (nb && nb->name[0] != '\0') {
        strncpy(s_name_buf, nb->name, sizeof(s_name_buf) - 1);
        s_name_buf[sizeof(s_name_buf) - 1] = '\0';
        xSemaphoreGive(s_state_mutex);
        return s_name_buf;
    }
    xSemaphoreGive(s_state_mutex);
    return NULL;
}

/* ── Traffic debug access ───────────────────────────────────────────── */

static void traffic_event_notify(const traffic_event_t* evt, void* ctx) {
    (void)ctx;

    /* Only send notifications if debug is enabled */
    if (!traffic_debug_is_enabled(&s_traffic_debug)) {
        return;
    }

    /* Build notification payload. Shared serializer so this and the
     * getTrafficEvents reply cannot drift apart. */
    cJSON* params = cJSON_CreateObject();
    traffic_event_add_json(params, evt);

    /* Send notification via RPC notify system (which forwards to WebSocket) */
    rpc_notify("bramble.onTrafficEvent", params);

    cJSON_Delete(params);
}

traffic_debug_t* mesh_get_traffic_debug(void) { return &s_traffic_debug; }

void mesh_traffic_debug_set_config(bool enabled, bool include_tx, bool include_rx,
                                   uint8_t sample_rate) {
    /* F26: Update in-memory state under mutex, then do NVS I/O outside */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    traffic_debug_enable(&s_traffic_debug, enabled);
    xSemaphoreGive(s_state_mutex);

    /* Persist config to NVS (flash I/O, do NOT hold mesh mutex) */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_TELEMETRY_DBG, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "enabled", enabled ? 1 : 0);
        nvs_set_u8(nvs, "inc_tx", include_tx ? 1 : 0);
        nvs_set_u8(nvs, "inc_rx", include_rx ? 1 : 0);
        nvs_set_u8(nvs, "sample", sample_rate);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Traffic debug %s", enabled ? "enabled" : "disabled");
}

void mesh_traffic_debug_get_config(bool* enabled, bool* include_tx, bool* include_rx,
                                   uint8_t* sample_rate) {
    /* F26: Read in-memory state under mutex, then do NVS I/O outside */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (enabled)
        *enabled = traffic_debug_is_enabled(&s_traffic_debug);
    xSemaphoreGive(s_state_mutex);

    /* Load other config from NVS (flash I/O, do NOT hold mesh mutex) */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_TELEMETRY_DBG, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t val = 0;
        if (include_tx && nvs_get_u8(nvs, "inc_tx", &val) == ESP_OK)
            *include_tx = (val != 0);
        else if (include_tx)
            *include_tx = true; /* default */

        if (include_rx && nvs_get_u8(nvs, "inc_rx", &val) == ESP_OK)
            *include_rx = (val != 0);
        else if (include_rx)
            *include_rx = true; /* default */

        if (sample_rate && nvs_get_u8(nvs, "sample", &val) == ESP_OK)
            *sample_rate = val;
        else if (sample_rate)
            *sample_rate = 100; /* default: no sampling */

        nvs_close(nvs);
    } else {
        /* NVS read failed, return defaults */
        if (include_tx)
            *include_tx = true;
        if (include_rx)
            *include_rx = true;
        if (sample_rate)
            *sample_rate = 100;
    }
}

void mesh_traffic_debug_load_config(void) {
    /* Called at startup to restore persisted config */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_TELEMETRY_DBG, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t enabled = 0;
        if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) {
            traffic_debug_enable(&s_traffic_debug, enabled != 0);
            ESP_LOGI(TAG, "Loaded traffic debug config: enabled=%d", enabled);
        }
        nvs_close(nvs);
    }
}
