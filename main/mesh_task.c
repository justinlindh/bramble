/**
 * Bramble mesh task — runs on CPU1, handles radio TX/RX and protocol dispatch.
 */

#include "mesh_task.h"
#include "util.h"
#include "rreq_pseudonym.h"
#include "beacon_policy_calc.h"
#include "probe_results.h"
#include "probe_reply.h"
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
static void rerr_fastfail_notify(uint32_t packet_id, const char* reason, void* ctx);
static void handle_ke_envelope(uint32_t src_addr, int channel_idx, const uint8_t* data,
                               size_t data_len);
/* ws 1.3b: defined near handle_beacon (below send_beacon in file order),
 * but send_beacon is the first control-plane originator in the file and
 * needs both before its own definition. */
static int control_seq_next(uint64_t* out);
static bool control_replay_ok(uint32_t signer_addr, uint64_t seq);
/* Task 5 (channel flood): handle_data (near the top of the file) schedules
 * a jittered rebroadcast of a broadcast DATA frame; the queue it schedules
 * onto is defined near its sibling schedule_rreq_forward, further down. */
static void schedule_flood_relay(const uint8_t* buf, uint8_t len, uint32_t jitter_ms,
                                 uint32_t flood_key, tx_kind_t tx_kind);

/* ── Configuration ──────────────────────────────────────────────────── */

#define BEACON_INTERVAL_MS 60000      /* 60 seconds between beacons (A/B test) */
#define BEACON_JITTER_MS 5000         /* ±5s random jitter */
#define NEIGHBOR_PURGE_INTERVAL 60000 /* purge expired neighbors every 60s */
#define RX_QUEUE_DEPTH 16
#define MESH_EVENT_QUEUE_DEPTH 8
#define MESH_TASK_STACK 8192
#define MESH_TASK_PRIORITY 5

#define RECEIPT_QUEUE_CAPACITY 8
#define RECENT_BROADCAST_RING_SIZE 8

/* ── Received packet queue item ─────────────────────────────────────── */

typedef struct {
    uint8_t data[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t len;
    int16_t rssi;
    int8_t snr;
} rx_packet_t;

/* ── State ──────────────────────────────────────────────────────────── */

static bramble_identity_t* s_identity;
static uint8_t s_beacon_key[BRAMBLE_KEY_SIZE]; /* shared key for beacon HMAC */
static neighbor_table_t s_neighbors;
static dedup_buffer_t s_dedup;
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
static dedup_buffer_t s_flood_dedup;
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
static replay_table_t s_replay; /* SEC-M1: per-sender authenticated nonce-counter replay window */
static replay_table_t s_control_replay; /* ws 1.3b: control-plane (RREP/RERR/ACK/receipt/beacon)
                                           replay window, keyed on the authenticated signer address,
                                           separate from the data-plane s_replay above */
/* Per-node identity Phase 3 (Part C): this node's verified TOFU pin table
 * (address -> Ed25519/X25519 pubs), fed by handle_identity_attestation
 * below. RAM only; pins reset on reboot and TOFU re-establishes. */
static identity_store_t s_identity_pins;
static replay_deferred_t s_deferred; /* tier-2: deferred acceptance for delayed CHAT (Task 0.6) */
/* RREQ origination gate. Forwarded RREQs are gated separately, by the global
 * s_rreq_fwd_rl budget below (ws 1.3d, SEC-M4); see SECURITY-MODEL.md for the
 * node-global-not-per-neighbor residual. */
static rreq_rate_limiter_t s_rreq_rl;
/* Global forwarded-RREQ token bucket (ws 1.3d, SEC-M4). Bounds this node's
 * aggregate forwarded-RREQ rate regardless of the unauthenticated, spoofable
 * rreq.prev_hop field; not keyed per-neighbor on purpose (see
 * SECURITY-MODEL.md). */
static rreq_fwd_limiter_t s_rreq_fwd_rl;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_delivery_event_mutex;
/* Guards nonce_counter_next only: send_data_packet is reachable from the
 * mesh task, the RPC/httpd task, the LVGL UI task, and the CLI task, all
 * without a lock of their own, and the nonce counter itself has no internal
 * locking (kept host-testable, no FreeRTOS dependency). A mutex, not a
 * critical section, because a boundary flush may block on NVS I/O. */
static SemaphoreHandle_t s_nonce_mutex;
/*
 * DM session table (SEC-C2, Task 1.4). Guards every dm_lookup/dm_alloc,
 * every session state transition, and every read of session_key for
 * dm_session_encrypt/decrypt. Reachable from the mesh RX task
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
static SemaphoreHandle_t s_dm_mutex;
static dm_table_t s_dm_table;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_mesh_event_queue;
static mesh_shared_state_t s_shared;

typedef enum {
    MESH_EVT_RECEIPT_TX = 1,
    MESH_EVT_PROBE_REPLY_TX = 2,
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

static pending_probe_reply_t s_probe_reply_queue[PROBE_REPLY_QUEUE_CAPACITY];
static esp_timer_handle_t s_probe_reply_timer;

static delivery_event_ring_t* s_delivery_event_ring;

enum {
    DELIVERY_EVENT_TYPE_ACK = 1,
    DELIVERY_EVENT_TYPE_BROADCAST_DELIVERY = 2,
};
static char s_node_name[BRAMBLE_NODE_NAME_MAX + 1] = ""; /* loaded from NVS at startup */

/* Routing state */
static routing_table_t s_routes;
static rreq_dedup_t s_rreq_dedup;
static reverse_route_table_t s_reverse_routes;
static pending_discovery_table_t s_pending_disc;

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
typedef struct {
    uint32_t dest_addr;
    uint8_t data[BRAMBLE_MAX_PACKET_SIZE];
    size_t len;
    uint32_t timestamp;
    bool used;
    uint8_t reason;  /* QUEUE_REASON_ROUTE or QUEUE_REASON_SESSION */
    uint32_t pkt_id; /* tracking id surfaced to the caller/UI; only meaningful for
                        QUEUE_REASON_SESSION */
    int16_t
        channel_idx; /* only meaningful for QUEUE_REASON_SESSION (msg_store bookkeeping on flush) */
} queued_msg_t;
static queued_msg_t s_queued_msgs[MAX_QUEUED_MSGS];

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
typedef struct {
    uint32_t src_addr;
    uint32_t eph_pub_hash;
    uint16_t ke_epoch;
    uint32_t seen_ms;
    bool used;
} dm_hs_dedup_entry_t;
static dm_hs_dedup_entry_t s_hs_dedup[DM_HS_DEDUP_MAX];

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
typedef struct {
    uint32_t peer_addr;
    uint8_t eph_priv[32];
    uint8_t eph_pub[32];
    bool used;
} dm_pending_eph_t;
static dm_pending_eph_t s_pending_eph[DM_MAX_HANDSHAKING];

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
typedef struct {
    uint32_t src_addr;
    int channel_idx;
    /* Self-heal (DM session desync): when true the worker INITIATES a fresh
     * handshake to src_addr instead of processing a received msg. Lets the
     * DH-heavy initiate_dm_handshake run off the mesh RX task (M7), the same
     * reason process_ke_init/resp do. handle_ke_envelope memsets its item to
     * 0, so received-KE work items leave this false. */
    bool initiate;
    bramble_key_exchange_t msg;
    /* Phase 4 DM key continuity: the pinned X25519 key for src_addr,
     * SNAPSHOTTED by handle_ke_envelope on the mesh task (the only task
     * that mutates s_identity_pins) so the worker never touches the pin
     * store cross-thread. have_pin false = no pin known (TOFU-grade). */
    bool have_pin;
    uint8_t pinned_x25519[32];
} dm_handshake_work_item_t;
static QueueHandle_t s_handshake_work_q;

/* Jittered channel-flood relay queue (Task 5). Same shape and drain cadence
 * as the RREQ forward queue below, holding the exact relay-mutated wire
 * bytes (hop_limit decremented, prev_hop rewritten to us) a broadcast DATA
 * frame is rebroadcast with once its jitter elapses. pending_flood_relay_t,
 * FLOOD_RELAY_QUEUE_CAPACITY and the rebroadcast-suppression helper live in
 * channel_flood.h (Flooding F1) so they are unit-testable in isolation. */
static pending_flood_relay_t s_flood_relay_queue[FLOOD_RELAY_QUEUE_CAPACITY];

/* Jittered RREQ forward queue (DES-3). Relays delay RREQ rebroadcasts by a
 * random 50-300ms so same-hop relays do not key up at the same instant; the
 * mesh task drains due entries from its main loop (10ms poll cadence). */
#define RREQ_FWD_QUEUE_CAPACITY 8
typedef struct {
    bool used;
    uint32_t due_at_ms;
    bramble_rreq_t rreq;
} pending_rreq_fwd_t;
static pending_rreq_fwd_t s_rreq_fwd_queue[RREQ_FWD_QUEUE_CAPACITY];

/* Reliability — ACK tracking for outgoing unicast messages */
static pending_ack_table_t s_pending_acks;

/* Traffic debug telemetry */
#define TRAFFIC_DEBUG_CAPACITY 512
static traffic_event_t s_traffic_events[TRAFFIC_DEBUG_CAPACITY];
static traffic_debug_t s_traffic_debug;
static timesync_state_t s_timesync;

/* Fragment reassembly context */
static reassembly_ctx_t s_reassembly;

/* Adaptive beacon interval policy */
static beacon_policy_config_t s_beacon_policy = {
    .enabled = false, /* Default: disabled (fixed 60s) */
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
static churn_sample_t s_churn_history[MAX_CHURN_HISTORY];
static int s_churn_history_idx = 0;

/* Channel state */
static bramble_channel_t s_channels[MAX_CHANNELS];
static char s_channel_names[MAX_CHANNELS][20];
static bool s_channel_has_psk[MAX_CHANNELS];
static int s_num_channels = 0;
static int s_default_channel_idx = 0; /* unicast default, public broadcast stays channel 0 */
static uint32_t s_last_broadcast_id = 0;
static uint16_t s_last_broadcast_frag_msg_id = 0;
static uint32_t s_recent_broadcast_ids[RECENT_BROADCAST_RING_SIZE];
static int s_recent_broadcast_idx = 0;
static broadcast_telemetry_mode_t s_broadcast_telemetry_mode = BROADCAST_TELEMETRY_RECIPIENT_ONLY;

/* Mailbox — store-and-forward for offline neighbors (backed by components/mailbox) */
static bool s_mailbox_enabled = false;
#define MAILBOX_BEACON_FLAG 0x01
static mailbox_t s_mailbox;

/* Flooding F1 Task 1: runtime toggle for the unicast flood transport. OFF
 * (default) preserves today's reactive route-lookup forward for unicast
 * DATA; ON routes unicast DATA not addressed to us through the same
 * multi-hop flood engine broadcast DATA already uses (channel_flood_decide +
 * s_flood_dedup) instead of forward_data_packet. See mesh_process_rx_packet's
 * PKT_TYPE_DATA case. NVS-persisted, same pattern as s_mailbox_enabled. */
static bool s_flood_transport = false;

/* Flooding F1 finalize: operator-settable flood-transport origination hop
 * budget. Under s_flood_transport, a freshly-originated flood DATA and its
 * flooded-ACK are stamped with this hop_limit (via flood_origination_hop_limit)
 * instead of the hardcoded ROUTE_HOP_LIMIT_MAX, so reach can be matched to the
 * expected network diameter. Default FLOOD_HOP_LIMIT_DEFAULT (8) leaves shipped
 * behavior unchanged; NVS-persisted (NVS_NS_FLOOD "hop_limit"); clamped to
 * [FLOOD_HOP_LIMIT_MIN, FLOOD_HOP_LIMIT_CEIL]. SEPARATE from ROUTE_HOP_LIMIT_MAX
 * (the reactive path is untouched). */
static uint8_t s_flood_hop_limit = FLOOD_HOP_LIMIT_DEFAULT;

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
static void handle_probe(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
static void handle_probe_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
static void handle_delivery_receipt(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
static int mesh_send_probe_round(uint32_t pid, uint8_t round);
static void mesh_start_probe_sweep(uint32_t pid);
static void mailbox_flush_for(uint32_t dest_addr);
static int mesh_tx(const uint8_t* buf, uint8_t len, tx_kind_t kind);
static void queue_broadcast_delivery_receipt(uint32_t original_src_addr,
                                             uint32_t original_packet_id);
static void mesh_schedule_next_receipt_timer(void);
static void mesh_process_receipt_tx_event(void);
static void mesh_receipt_timer_cb(void* arg);
static void mesh_schedule_next_probe_reply_timer(void);
static void mesh_process_probe_reply_tx_event(void);
static void mesh_probe_reply_timer_cb(void* arg);
static void queue_probe_reply(const uint8_t* buf, uint8_t wire_len, uint32_t address);
static void mesh_persist_channel_psk_flags(void);
static void mesh_load_channel_psk_flags(void);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void delivery_event_ring_append_locked(const delivery_event_record_t* event) {
    if (!event || !s_delivery_event_mutex || !s_delivery_event_ring)
        return;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    delivery_event_ring_append(s_delivery_event_ring, event);
    xSemaphoreGive(s_delivery_event_mutex);
}

static void record_ack_delivery_event(const bramble_ack_t* ack) {
    if (!ack)
        return;

    delivery_event_record_t evt = {0};
    evt.message_id = ack->ack_packet_id;
    evt.timestamp_s = now_ms() / 1000u;
    evt.recipient_addr = ack->src_addr;
    evt.source_addr = s_identity ? s_identity->address : 0u;
    evt.event_type = DELIVERY_EVENT_TYPE_ACK;
    evt.tier = MSG_TIER_NORMAL;

    uint8_t hops = ack->hop_count;
    if (hops > DELIVERY_EVENT_ROUTE_MAX_HOPS)
        hops = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    evt.route_len = hops;
    for (uint8_t i = 0; i < hops; i++) {
        evt.route_hops[i] = ack->relay_path[i];
    }

    delivery_event_ring_append_locked(&evt);
}

static void record_broadcast_delivery_event(uint32_t recipient_addr, uint32_t broadcast_id,
                                            uint8_t hop_count, const uint32_t* relay_path) {
    delivery_event_record_t evt = {0};
    evt.message_id = broadcast_id;
    evt.timestamp_s = now_ms() / 1000u;
    evt.recipient_addr = recipient_addr;
    evt.source_addr = s_identity ? s_identity->address : 0u;
    evt.event_type = DELIVERY_EVENT_TYPE_BROADCAST_DELIVERY;
    evt.tier = MSG_TIER_BROADCAST;

    uint8_t hops = hop_count;
    if (hops > DELIVERY_EVENT_ROUTE_MAX_HOPS)
        hops = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    evt.route_len = hops;
    if (relay_path) {
        for (uint8_t i = 0; i < hops; i++) {
            evt.route_hops[i] = relay_path[i];
        }
    }

    delivery_event_ring_append_locked(&evt);
}

static void recent_broadcast_record(uint32_t packet_id) {
    if (packet_id == 0)
        return;
    s_recent_broadcast_ids[s_recent_broadcast_idx] = packet_id;
    s_recent_broadcast_idx = (s_recent_broadcast_idx + 1) % RECENT_BROADCAST_RING_SIZE;
}

static bool recent_broadcast_contains(uint32_t packet_id) {
    if (packet_id == 0)
        return false;
    for (int i = 0; i < RECENT_BROADCAST_RING_SIZE; i++) {
        if (s_recent_broadcast_ids[i] == packet_id) {
            return true;
        }
    }
    return false;
}

static void maybe_emit_implicit_broadcast_delivery(const bramble_header_t* header,
                                                   const rx_packet_t* pkt) {
    if (!header || !pkt)
        return;
    if (header->type != PKT_TYPE_DATA)
        return;
    if (header->dest_addr != 0xFFFFFFFFu)
        return;
    if (!recent_broadcast_contains(header->packet_id))
        return;
    if (pkt->len < HEADER_SIZE + sizeof(uint32_t))
        return;

    uint32_t relayer_addr = 0;
    memcpy(&relayer_addr, pkt->data + HEADER_SIZE, sizeof(relayer_addr));
    if (relayer_addr == 0)
        return;

    uint32_t relay_path[1] = {relayer_addr};
    mesh_emit_broadcast_delivery_notification(relayer_addr, header->packet_id, 0, 1, relay_path);
    record_broadcast_delivery_event(relayer_addr, header->packet_id, 1, relay_path);
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

static void location_policy_load_or_defaults(nvs_handle_t nvs, location_policy_t* policy) {
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
    if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) != ESP_OK) {
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

uint32_t mesh_send_location_packet(uint32_t dest_addr, const bramble_position_t* pos,
                                   uint8_t tier) {
    if (!pos || !pos->valid)
        return 0;

    if (tier > LOCATION_TIER_PRESENCE) {
        tier = LOCATION_TIER_COARSE;
    }

    /* SEC-C1: tier moves into the encrypted plaintext (byte
     * LOCATION_INNER_TIER_OFFSET), padded to L_LOC_INNER so every tier
     * (PRESENCE/COARSE/FULL) produces an identical ciphertext length; an
     * observer cannot infer the tier from packet size. Zeroed first so
     * unused padding is deterministic, not stack garbage. */
    uint8_t inner[L_LOC_INNER] = {0};
    inner[LOCATION_INNER_TIER_OFFSET] = tier;
    if (location_serialize_for_tier(pos, tier, inner + 1, LOCATION_FULL_SIZE) <= 0) {
        return 0;
    }

    uint32_t pkt_id = next_packet_id();
    uint8_t pkt[BRAMBLE_MAX_PACKET_SIZE] = {0};
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    if (dest_addr != 0xFFFFFFFFu) {
        /* Directed share (lcr_<addr>): only ever under the recipient's
         * session key, never the channel key (would defeat per-contact
         * confidentiality, the SEC-C1 point). No ACTIVE session means the
         * send fails rather than downgrading to the channel key: location
         * is real-time presence (RFC M6, never mailbox-deferred), so
         * queuing this to await a handshake the way DM chat does (Task
         * 1.4) would only deliver a stale position later, not a
         * meaningful fix. */
        bramble_header_t header = {
            .version = BRAMBLE_VERSION,
            .type = PKT_TYPE_LOCATION,
            .flags = FLAG_ENCRYPT, /* no FLAG_CHANNEL: session-keyed (SEC-C1) */
            .hop_limit = 3,
            .dest_addr = dest_addr,
            .packet_id = pkt_id,
        };
        bramble_header_serialize(&header, pkt, HEADER_SIZE);

        /* dm_session_encrypt has no framing of its own, so pad out to
         * L_LOC_INNER + CHANNEL_MSG_OVERHEAD bytes: the extra padding
         * makes up for the channel path's built-in overhead below, so
         * both paths land on the exact same total ciphertext length
         * (M11). */
        uint8_t session_inner[L_LOC_INNER + CHANNEL_MSG_OVERHEAD] = {0};
        memcpy(session_inner, inner, L_LOC_INNER);
        uint8_t ciphertext[sizeof(session_inner)];

        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(&s_dm_table, dest_addr);
        int enc_ret = -1;
        if (sess && sess->state == DM_STATE_ACTIVE) {
            xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
            int nonce_ret = nonce_counter_next(nonce);
            xSemaphoreGive(s_nonce_mutex);
            if (nonce_ret == 0) {
                enc_ret = dm_session_encrypt(sess, &header, s_identity->address, session_inner,
                                             sizeof(session_inner), nonce, ciphertext, tag);
                if (enc_ret == 0)
                    sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
            }
        }
        xSemaphoreGive(s_dm_mutex);

        if (enc_ret != 0) {
            ESP_LOGW(TAG, "No active session for directed location share to %08" PRIX32, dest_addr);
            return 0;
        }

        memcpy(pkt + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
        /* Wire v4: originator writes its own address as prev_hop, same as
         * send_data_packet/send_dm_packet. */
        memcpy(pkt + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
        /* Wire v4 (F1): origin-authenticate; see send_data_packet. LOCATION
         * shares the envelope so it carries the field, though it is never
         * relayed today (handle_location delivers dest==self/broadcast only).
         * Mandatory-provisioning (Task 2): abort if unprovisioned. */
        if (data_auth_sign(&header, s_identity->address, pkt + BRAMBLE_DATA_AUTH_HMAC_OFFSET) !=
            0) {
            ESP_LOGD(TAG, "unprovisioned: inert, dropping location (session) send");
            return 0;
        }
        memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
        memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext,
               sizeof(ciphertext));
        memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + sizeof(ciphertext), tag,
               BRAMBLE_TAG_SIZE);
        size_t wire_len = BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE +
                          sizeof(ciphertext) + BRAMBLE_TAG_SIZE;

        int rc = mesh_tx(pkt, (uint8_t)wire_len, TX_KIND_DATA);
        if (rc == TX_GATE_OK) {
            ESP_LOGI(TAG, "TX location (session) to %08" PRIX32 " tier=%u len=%u", dest_addr, tier,
                     (unsigned)wire_len);
            return pkt_id;
        }
        return 0;
    }

    /* Channel-shared (broadcast): channel_msg_encrypt under the default
     * channel key. */
    if (s_num_channels == 0) {
        return 0;
    }
    int channel_idx = s_default_channel_idx;
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        channel_idx = 0;
    }

    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_LOCATION,
        .flags = FLAG_ENCRYPT | FLAG_CHANNEL, /* no tier bits: tier lives in the ciphertext now */
        .hop_limit = 3,
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };
    bramble_header_serialize(&header, pkt, HEADER_SIZE);

    uint8_t aad[HEADER_SIZE + 4];
    bramble_build_aead_aad(&header, s_identity->address, aad, sizeof(aad));

    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    xSemaphoreGive(s_nonce_mutex);
    if (nonce_ret != 0) {
        ESP_LOGE(TAG, "Nonce counter unavailable, dropping location send: %d", nonce_ret);
        return 0;
    }

    uint8_t ciphertext[CHANNEL_MSG_OVERHEAD + L_LOC_INNER];
    if (channel_msg_encrypt(&s_channels[channel_idx], s_identity->address, APP_TYPE_LOCATION, 0,
                            inner, L_LOC_INNER, aad, sizeof(aad), nonce, ciphertext, tag) != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed for location send");
        return 0;
    }

    memcpy(pkt + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
    /* Wire v4: originator writes its own address as prev_hop. */
    memcpy(pkt + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    /* Wire v4 (F1): origin-authenticate; see send_data_packet. Mandatory-
     * provisioning (Task 2): abort if unprovisioned. */
    if (data_auth_sign(&header, s_identity->address, pkt + BRAMBLE_DATA_AUTH_HMAC_OFFSET) != 0) {
        ESP_LOGD(TAG, "unprovisioned: inert, dropping location (channel) send");
        return 0;
    }
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext, sizeof(ciphertext));
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + sizeof(ciphertext), tag,
           BRAMBLE_TAG_SIZE);

    size_t wire_len = BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + sizeof(ciphertext) +
                      BRAMBLE_TAG_SIZE;
    int rc = mesh_tx(pkt, (uint8_t)wire_len, TX_KIND_DATA);
    if (rc == TX_GATE_OK) {
        ESP_LOGI(TAG, "TX location (channel) to %08" PRIX32 " tier=%u len=%u", dest_addr, tier,
                 (unsigned)wire_len);
        return pkt_id;
    }
    return 0;
}

static void mesh_emit_location_event(const char* event, uint32_t peer_addr, uint8_t tier,
                                     uint32_t timestamp_ms, int16_t rssi, int8_t snr,
                                     uint32_t count) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return;
    cJSON_AddStringToObject(params, "event", event);
    if (peer_addr != 0) {
        char addr_buf[9];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, peer_addr);
        cJSON_AddStringToObject(params, "peer", addr_buf);
    }
    cJSON_AddStringToObject(params, "tier", location_tier_to_string(tier));
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

static void mesh_send_location_updates(uint32_t t, const location_policy_t* policy,
                                       const bramble_position_t* source_pos) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    bramble_position_t pos = *source_pos;
    pos.timestamp = t / 1000;
    pos.valid = true;

    uint32_t sent_count = 0;
    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) == ESP_OK) {
        while (it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            if (strncmp(info.key, "lcr_", 4) == 0) {
                const char* addr = info.key + 4;

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
                    uint32_t pkt_id =
                        mesh_send_location_packet((uint32_t)strtoul(addr, NULL, 16), &pos, tier);
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

static void mesh_persist_peer_location(uint32_t peer_addr, const bramble_position_t* pos,
                                       uint8_t tier, uint32_t now_ms) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
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

/*
 * SEC-C1 RX channel-path glue (Task 2.2): trial-decrypts against the known
 * channels, then hands the resulting plaintext to location_parse_inner
 * (decrypt-mechanism-agnostic tier + position parsing, exported by the
 * location component; see its own comment for why it stays dependency-free
 * rather than owning this glue itself). This function is intentionally
 * thin: the only logic worth testing (tier extraction, tier-appropriate
 * position parsing) lives in location_parse_inner, already covered by
 * test_location_crypto.c.
 */
static int location_rx_decode_channel(const uint8_t* nonce, const uint8_t* ciphertext,
                                      size_t ct_len, const uint8_t* tag, const uint8_t* aad,
                                      size_t aad_len, uint8_t* tier_out,
                                      bramble_position_t* pos_out, int* channel_index_out) {
    uint8_t plaintext[CHANNEL_MSG_MAX_PLAINTEXT_SIZE];
    channel_msg_info_t info;
    if (channel_msg_decrypt(s_channels, s_num_channels, nonce, ciphertext, ct_len, tag, aad,
                            aad_len, plaintext, &info, now_ms()) != 0) {
        return -1;
    }
    *channel_index_out = info.channel_index;
    return location_parse_inner(info.data, info.data_len, tier_out, pos_out);
}

static void handle_location(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    /* SEC-C1 RX (Task 2.2): location packet layout matches DATA's wire v4
     * envelope: header(12) + src_addr(4) + prev_hop(4) + nonce(12) +
     * ciphertext(N) + tag(16). LOCATION is never forwarded today (no relay
     * path exists for it), so prev_hop is written by the originator only
     * and not consulted for reverse-route learning here; see
     * task-4-report.md for why that is in scope but deliberately not
     * turned on. */
    if (len < BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE + 1) {
        ESP_LOGW(TAG, "Location packet too short: %u", len);
        return;
    }

    uint32_t src_addr = 0;
    memcpy(&src_addr, data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
    if (src_addr == s_identity->address) {
        return;
    }

    bramble_header_t header;
    if (bramble_header_deserialize(&header, data, len) != ESP_OK) {
        return;
    }

    const uint8_t* nonce = data + BRAMBLE_DATA_NONCE_OFFSET;
    size_t ct_len = len - BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;
    const uint8_t* ciphertext = nonce + BRAMBLE_NONCE_SIZE;
    const uint8_t* tag = ciphertext + ct_len;

    if (ct_len > BRAMBLE_MAX_PACKET_SIZE) {
        ESP_LOGW(TAG, "Location ciphertext too large: %u", (unsigned)ct_len);
        return;
    }

    uint8_t aad[HEADER_SIZE + 4];
    bramble_build_aead_aad(&header, src_addr, aad, sizeof(aad));

    /* Discriminator mirrors handle_data (SEC-C1/SEC-C2 share the same
     * mechanism): FLAG_CHANNEL set means channel-shared, trial-decrypt
     * against known channels; absent means a directed share, look up the
     * ACTIVE session for src_addr under s_dm_mutex. */
    uint8_t tier = 0;
    bramble_position_t pos = {0};
    int ok = -1;
    int is_channel_message = (header.flags & FLAG_CHANNEL) ? 1 : 0;
    int channel_index = 0;

    if (is_channel_message) {
        ok = location_rx_decode_channel(nonce, ciphertext, ct_len, tag, aad, sizeof(aad), &tier,
                                        &pos, &channel_index);
    } else {
        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(&s_dm_table, src_addr);
        if (sess && sess->state == DM_STATE_ACTIVE) {
            /* Canonical session-path size (Task 2.1, M11): the encoder
             * always pads to exactly L_LOC_INNER + CHANNEL_MSG_OVERHEAD
             * bytes to match the channel path's total ciphertext length.
             * Reject anything else outright rather than risk decrypting
             * into an undersized buffer. */
            uint8_t plaintext[L_LOC_INNER + CHANNEL_MSG_OVERHEAD];
            if (ct_len == sizeof(plaintext) &&
                dm_session_decrypt(sess, &header, src_addr, nonce, ciphertext, ct_len, tag,
                                   plaintext) == 0) {
                ok = location_parse_inner(plaintext, sizeof(plaintext), &tier, &pos);
                sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
            }
        }
        xSemaphoreGive(s_dm_mutex);
    }

    if (ok != 0) {
        ESP_LOGW(TAG, "Failed to decrypt/parse location from %08" PRIX32, src_addr);
        return;
    }

    /* SEC-M1/M6: the SAME node-global replay window DATA uses (Task 0.5):
     * one nonce counter and one replay window per sender, shared across
     * every packet type that sender's node encrypts, since a counter is
     * only ever used once regardless of what it authenticates. Never
     * consults the deferred cache (that is chat-only, Task 0.6): location
     * is real-time presence, so both REPLAY_REJECT_DUP and
     * REPLAY_BELOW_WINDOW are dropped identically, never accepted late.
     * Fix 2 (red-team panel): skip this SHARED window entirely for a
     * public-channel decrypt, whose src_addr is a free-to-forge claim
     * (BRAMBLE_PUBLIC_CHANNEL_PSK is public), same reasoning and same
     * helper as handle_data. Public-channel location updates rely on the
     * pre-existing packet_id/type dedup for loop suppression instead. */
    uint64_t rx_counter = nonce_counter_extract(nonce);
    if (channel_source_is_replay_trustworthy(is_channel_message, channel_index)) {
        int rp = replay_check_and_add(&s_replay, src_addr, rx_counter, now_ms());
        if (rp != REPLAY_ACCEPT) {
            ESP_LOGD(TAG, "Location replay drop from %08" PRIX32 " ctr=%llu (rp=%d)", src_addr,
                     (unsigned long long)rx_counter, rp);
            return;
        }
    }

    uint32_t t = now_ms();
    location_cache_update(&s_location_mgr, src_addr, &pos, t);
    mesh_persist_peer_location(src_addr, &pos, tier, t);

    ESP_LOGI(TAG, "RX location from %08" PRIX32 " tier=%u RSSI:%d SNR:%d", src_addr, tier, rssi,
             snr);
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
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
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
    if (nvs_open(NVS_NS_CHANNEL, NVS_READWRITE, &h) != ESP_OK) {
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
    if (nvs_open(NVS_NS_CHANNEL, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    for (int i = 0; i < s_num_channels; i++) {
        /* Missing metadata defaults to "no PSK lock" for deterministic export semantics. */
        uint8_t has_psk = 0;
        /* Sized for a full int, so -Wformat-truncation holds on every target
         * (NVS keys allow up to 15 chars). */
        char key[16];
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
    if (delay_ms <= 0)
        delay_ms = 100;
    TimerHandle_t t =
        xTimerCreate("reboot", pdMS_TO_TICKS(delay_ms), pdFALSE, NULL, reboot_timer_cb);
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

static void on_tx_done(void) { ESP_LOGD(TAG, "TX complete"); }

/* ── Beacon TX ──────────────────────────────────────────────────────── */

static int send_beacon(void) {
    /* Mandatory-provisioning (Task 2): an unprovisioned node is INERT. It has
     * no beacon key (mesh_rederive_beacon_key zeroes it) and must emit no
     * network-key-authenticated frame, so skip the beacon entirely. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping beacon");
        return -1;
    }

    bramble_beacon_t beacon = {0};

    beacon.header.version = BRAMBLE_VERSION;
    beacon.header.type = PKT_TYPE_BEACON;
    beacon.header.flags = 0;
    beacon.header.hop_limit = 1;          /* beacons are 1-hop only */
    beacon.header.dest_addr = 0xFFFFFFFF; /* broadcast */
    beacon.header.packet_id = next_packet_id();

    beacon.src_addr = s_identity->address;
    beacon.pubkey_hash = s_identity->pubkey_hash;
    beacon.uptime_min = (uint16_t)(now_ms() / 60000);
    beacon.battery_pct = battery_read_pct();
    beacon.tx_queue_depth = 0;
    beacon.neighbor_count = (uint8_t)neighbor_count(&s_neighbors);
    beacon.flags = s_mailbox_enabled ? MAILBOX_BEACON_FLAG : 0;
    /* Timesync: piggyback network time on beacon when synchronized */
    if (s_timesync.synchronized) {
        beacon.network_time = (uint32_t)timesync_get_network_time(&s_timesync, now_ms());
        beacon.time_confidence = timesync_get_stratum(&s_timesync);
    } else {
        beacon.network_time = 0;
        beacon.time_confidence = 0xFFFF; /* no confidence */
    }

    /* Include node name in beacon (if set) */
    if (s_node_name[0] != '\0') {
        beacon.name_len = (uint8_t)strlen(s_node_name);
        if (beacon.name_len > BEACON_NAME_MAX)
            beacon.name_len = BEACON_NAME_MAX;
        memcpy(beacon.name, s_node_name, beacon.name_len);
        beacon.name[beacon.name_len] = '\0';
    }

    /* ws 1.3b: draw the 48-bit origin seq before the HMAC, since seq lives
     * inside the HMAC-covered prefix. Fail-closed: no seq means this
     * interval's beacon doesn't go out; the next scheduled beacon tries
     * again. */
    uint64_t beacon_seq;
    if (control_seq_next(&beacon_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping beacon this interval");
        return -1;
    }
    beacon.seq[0] = (uint8_t)(beacon_seq >> 40);
    beacon.seq[1] = (uint8_t)(beacon_seq >> 32);
    beacon.seq[2] = (uint8_t)(beacon_seq >> 24);
    beacon.seq[3] = (uint8_t)(beacon_seq >> 16);
    beacon.seq[4] = (uint8_t)(beacon_seq >> 8);
    beacon.seq[5] = (uint8_t)beacon_seq;

    /* HMAC auth — use shared beacon key (derived from public channel PSK) */
    beacon_compute_hmac(&beacon, s_beacon_key, sizeof(s_beacon_key));

    /* Red-team fix: was buf[64], a hand-counted constant that predates the
     * ws 1.3b size bumps. BEACON_SIZE + 1 + BEACON_NAME_MAX (the max wire
     * size with a full-length name) is 71 as of BEACON_SIZE 54, so any
     * name of 10+ characters overflowed this buffer, bramble_beacon_
     * serialize's own len < need guard rejected it, and the node silently
     * stopped beaconing entirely (no neighbor announce, mailbox flush, or
     * timesync) until the name was cleared. Same size expression
     * beacon_compute_hmac already uses for its own buffer, not a new
     * magic number. */
    uint8_t buf[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    if (bramble_beacon_serialize(&beacon, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Beacon serialize failed");
        return -1;
    }

    size_t beacon_wire_len = bramble_beacon_wire_size(&beacon);
    /* Register the live beacon size with the gate: it funds the beacon
     * reserve (one beacon ToA held back from broadcast-data spenders) and
     * the budget-derived minimum interval used by the scheduler below. */
    tx_gate_set_beacon_size((uint8_t)beacon_wire_len);
    /* Beacons fit the budget by design (reserve + stretched interval);
     * denial is the never-expected backstop and only logs. */
    int ret = mesh_tx(buf, (uint8_t)beacon_wire_len, TX_KIND_BEACON);
    if (ret == TX_GATE_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.beacon_tx_count++;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Beacon TX #%" PRIu32 " (neighbors: %d)", s_shared.beacon_tx_count,
                 neighbor_count(&s_neighbors));
    } else if (ret == TX_GATE_ERR_BUDGET) {
        ESP_LOGD(TAG, "Beacon skipped this interval: airtime budget exhausted");
    } else {
        ESP_LOGE(TAG, "Beacon TX failed: %d", ret);
    }
    return ret;
}

/* ── Identity attestation TX (per-node identity Phase 2) ────────────── */

/* Low-cadence self-signed identity broadcast: 230 bytes (the relay-gated
 * frame carrying the endorsement cert, IDENTITY_ATTESTATION_SIZE) every 15
 * minutes is the design's approved airtime budget. At the shipping profiles
 * one frame is 2123.8 us (LONG_RANGE SF10/125k) or 181.9 us (MEDIUM_RANGE
 * SF7/250k), so the per-node duty at this cadence is ~0.236% and ~0.0202%
 * respectively (computed via components/radio/radio_airtime.c; the trust-anchor
 * cert grew the frame 158 -> 230, a ~40% airtime bump that stays negligible).
 * Do not raise the cadence without re-flagging that budget. */
#define ATTESTATION_INTERVAL_MS (15u * 60u * 1000u)
/* Short retry after a failed/denied send, so a boot-time budget denial
 * does not leave the node unattested for a full interval. */
#define ATTESTATION_RETRY_MS 60000u

static uint32_t s_attestation_last_ms;
static uint32_t s_attestation_wait_ms; /* 0 = boot send not attempted yet */

/*
 * Build, self-sign and broadcast this node's identity attestation
 * (PKT_TYPE_IDENTITY_ATTESTATION): {address, X25519 pub, Ed25519 pub}
 * signed by the node's OWN Ed25519 key over the canonical message
 * bramble_identity_attestation_signed_msg builds (packet.h), then
 * relay-gated under the network-key MAC (Phase 3, ident_relay_sign): the
 * Ed25519 sig carries the claim's truth, the MAC carries relay privilege
 * (see the struct comment in packet.h). Ordering is load-bearing: seq is
 * drawn and the Ed25519 sig computed BEFORE ident_relay_sign, because the
 * MAC covers both. seq is drawn once here at origination and never
 * re-drawn by relays (the frame floods unmodified except hop_limit).
 *
 * Broadcast on the BROADCAST budget lane (TX_KIND_DATA_BROADCAST, the
 * same tier the beacon and the flood relay debit). hop_limit uses the
 * same origination helper as flood DATA/ACK sends: ROUTE_HOP_LIMIT_MAX
 * reactive, the configured flood hop budget under s_flood_transport.
 */
static int send_identity_attestation(void) {
    if (!s_identity)
        return -1;

    /* Mandatory-provisioning (Task 2): inert when unprovisioned. The relay-gate
     * MAC (ident_relay_sign) requires the network key; emit nothing without it. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping identity attestation");
        return -1;
    }

    /* ws 1.3b pattern (send_ack): draw the 48-bit origin seq up front,
     * fail-closed. No seq means no attestation goes out; the retry timer
     * covers it exactly like a budget denial. */
    uint64_t att_seq;
    if (control_seq_next(&att_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, skipping identity attestation");
        return -1;
    }

    bramble_identity_attestation_t att = {0};
    att.header.version = BRAMBLE_VERSION;
    att.header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    att.header.flags = 0;
    att.header.hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit);
    att.header.dest_addr = 0xFFFFFFFF; /* broadcast */
    att.header.packet_id = next_packet_id();

    att.src_addr = s_identity->address;
    memcpy(att.x25519_pub, s_identity->public_key, sizeof(att.x25519_pub));
    memcpy(att.ed25519_pub, s_identity->ed25519_public_key, sizeof(att.ed25519_pub));

    /* Endorsement cert (trust-anchor campaign, P1): carry our own cert when we
     * have one, else leave the zero-initialized fields (not_after == 0 ==
     * "no cert"). Set before ident_relay_sign below, which MACs the cert. The
     * cert is NOT part of the Ed25519 self-signature (that stays the 84-byte
     * canonical message); it is the anchor's signature, verified by receivers
     * in a later phase. */
    identity_endorsement_get(&att.not_after, att.endorsement_sig);

    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    if (bramble_identity_attestation_signed_msg(&att, msg, sizeof(msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Attestation message build failed");
        return -1;
    }
    if (crypto_ed25519_sign(s_identity->ed25519_private_key, msg, sizeof(msg), att.sig) != 0) {
        ESP_LOGE(TAG, "Attestation sign failed");
        return -1;
    }

    /* Relay gate (Phase 3): write seq, then MAC. Both after the Ed25519
     * sign above, since the MAC covers sig and seq. */
    att.seq[0] = (uint8_t)(att_seq >> 40);
    att.seq[1] = (uint8_t)(att_seq >> 32);
    att.seq[2] = (uint8_t)(att_seq >> 24);
    att.seq[3] = (uint8_t)(att_seq >> 16);
    att.seq[4] = (uint8_t)(att_seq >> 8);
    att.seq[5] = (uint8_t)att_seq;
    ident_relay_sign(&att);

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    if (bramble_identity_attestation_serialize(&att, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Attestation serialize failed");
        return -1;
    }

    int ret = mesh_tx(buf, IDENTITY_ATTESTATION_SIZE, TX_KIND_DATA_BROADCAST);
    if (ret == TX_GATE_OK) {
        ESP_LOGI(TAG, "Identity attestation TX (addr=%08" PRIX32 ")", att.src_addr);
    } else if (ret == TX_GATE_ERR_BUDGET) {
        ESP_LOGD(TAG, "Identity attestation deferred: airtime budget exhausted");
    } else {
        ESP_LOGE(TAG, "Identity attestation TX failed: %d", ret);
    }
    return ret;
}

/* Send now and schedule the next attempt: the full interval on success,
 * the short retry on budget denial / radio failure. Called from the
 * post-boot send hook, the periodic maintenance tick, and identity
 * regeneration (so a new identity is announced promptly). */
static void attempt_identity_attestation(uint32_t t) {
    int rc = send_identity_attestation();
    s_attestation_last_ms = t;
    s_attestation_wait_ms = (rc == TX_GATE_OK) ? ATTESTATION_INTERVAL_MS : ATTESTATION_RETRY_MS;
}

/*
 * ws 1.3b infra: control-plane seq draw + replay check. No callers yet
 * (tasks 2-6 wire these into the five control-plane build/handle sites);
 * kept static like every other file-local helper here, so an unused-function
 * warning is expected and harmless until those callers land.
 *
 * control_seq_next mirrors the data-plane nonce draw above (e.g.
 * send_data_packet): take s_nonce_mutex, call nonce_counter_next, and on
 * failure release the mutex and fail closed without touching *out. The
 * control plane doesn't need a full 12-byte AEAD nonce, just the 48-bit
 * counter nonce_counter_extract pulls out of it.
 */
static int control_seq_next(uint64_t* out) {
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    if (nonce_ret != 0) {
        xSemaphoreGive(s_nonce_mutex);
        return nonce_ret;
    }
    *out = nonce_counter_extract(nonce);
    xSemaphoreGive(s_nonce_mutex);
    return 0;
}

/*
 * ws 1.3b infra: control-plane replay check, fed only after a MAC verify
 * passes so signer_addr/seq are authenticated. Separate table from the
 * data-plane s_replay (see s_control_replay above).
 */
static bool control_replay_ok(uint32_t signer_addr, uint64_t seq) {
    return replay_check_and_add(&s_control_replay, signer_addr, seq, now_ms()) == REPLAY_ACCEPT;
}

/* ── Packet handlers ────────────────────────────────────────────────── */

static void handle_beacon(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    /* Mandatory-provisioning (Task 2): an unprovisioned node has no beacon key
     * (mesh_rederive_beacon_key zeroes it), so it cannot authenticate a beacon.
     * Drop before any verify/effect: accepting one would mean trusting an HMAC
     * over an all-zero key (a forgery). Fail closed, accept nothing. */
    if (!network_key_is_provisioned()) {
        return;
    }

    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid beacon (len=%u)", len);
        return;
    }

    /* Ignore our own beacons */
    if (beacon.src_addr == s_identity->address)
        return;

    /* Verify beacon HMAC authenticity using shared beacon key */
    if (!beacon_verify_hmac(&beacon, s_beacon_key, sizeof(s_beacon_key))) {
        ESP_LOGW(TAG, "Beacon from %08" PRIX32 " failed HMAC verification, discarding",
                 beacon.src_addr);
        return;
    }

    /* ws 1.3b: replay check on the authenticated signer (beacon.src_addr
     * is HMAC-covered, so an attacker cannot dodge the window by mutating
     * it). Checked immediately after HMAC verify and strictly before every
     * effect below: address-collision handling, neighbor_update, name
     * store, and timesync_handle_sync. Gating timesync closes the part of
     * NEW-SEC-4 where a replayed beacon re-feeds stale network_time; the
     * bootstrap-quorum race (1.3c) is closed separately by the bounded
     * per-boot grace in identity_store_quorum_eligible. */
    uint64_t beacon_seq = ((uint64_t)beacon.seq[0] << 40) | ((uint64_t)beacon.seq[1] << 32) |
                          ((uint64_t)beacon.seq[2] << 24) | ((uint64_t)beacon.seq[3] << 16) |
                          ((uint64_t)beacon.seq[4] << 8) | (uint64_t)beacon.seq[5];
    if (!control_replay_ok(beacon.src_addr, beacon_seq)) {
        ESP_LOGW(TAG, "Beacon replay src=%08" PRIX32, beacon.src_addr);
        return;
    }

    /* Check for address collision — different pubkey_hash but same address */
    if (identity_check_collision(s_identity, beacon.src_addr, beacon.pubkey_hash)) {
        ESP_LOGE(TAG, "ADDRESS COLLISION with %08" PRIX32 " — regenerating identity!",
                 beacon.src_addr);
        /* Regenerate keypair and persist to NVS */
        if (identity_generate_and_save(s_identity) != 0) {
            /* Entropy not ready (pre-RF window, SEC-L1): identity_generate_and_save
             * refused to persist and left s_identity fully untouched (see
             * crypto_generate_identity). Do NOT report a new identity that was
             * never actually generated. */
            ESP_LOGW(TAG, "identity regeneration deferred: entropy not ready");
            return;
        }
        ESP_LOGW(TAG, "New identity: %08" PRIX32, s_identity->address);
        /* Announce the regenerated identity promptly (Phase 2): new
         * address + keys mean the old attestation no longer describes
         * this node. Budget-gated like every attestation send. */
        attempt_identity_attestation(now_ms());
        /* Notify webapp */
        cJSON* params = cJSON_CreateObject();
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
    int idx =
        neighbor_update(&s_neighbors, beacon.src_addr, (int8_t)rssi, snr, beacon.pubkey_hash, t);
    int new_count = neighbor_count(&s_neighbors);
    bool is_new_peer = (new_count > old_count);

    /* Store peer name if present */
    if (idx >= 0 && beacon.name_len > 0) {
        memcpy(s_neighbors.entries[idx].name, beacon.name, beacon.name_len);
        s_neighbors.entries[idx].name[beacon.name_len] = '\0';
    } else if (idx >= 0) {
        s_neighbors.entries[idx].name[0] = '\0';
    }

    /* Feed timesync from beacon — requires corroboration from multiple sources */
    if (beacon.network_time != 0 && beacon.time_confidence != 0xFFFF) {
        /* ws 1.3c: only established neighbors count toward the pre-commit
         * corroboration quorum (NEW-SEC-4 anti-Sybil lever). Computed after
         * neighbor_update above so the current beacon's tenure (beacon_count,
         * first_seen_ms) is reflected before the established check.
         *
         * Phase 4 identity gate on top: a PINNED peer always corroborates
         * (a fabricated source address cannot be pinned post-rebind: it has
         * no deriving Ed key); an UNPINNED peer corroborates only within the
         * bounded per-boot bootstrap grace (QUORUM_BOOTSTRAP_GRACE_MS) so a
         * fresh mesh still converges, and NEVER after it (NEW-SEC-4 1.3c
         * bootstrap-quorum race closed). Full semantics + tests:
         * identity_store_quorum_eligible (identity_store.h). Runs on the
         * same task as handle_identity_attestation, so no locking. */
        bool established = neighbor_is_established(&s_neighbors, beacon.src_addr, t);
        bool quorum_ok =
            identity_store_quorum_eligible(&s_identity_pins, beacon.src_addr, established, t);
        timesync_handle_sync(&s_timesync, (int64_t)beacon.network_time,
                             (uint8_t)beacon.time_confidence, beacon.src_addr, quorum_ok, t);
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.beacon_rx_count++;
    s_shared.last_rx_rssi = rssi;
    s_shared.last_rx_snr = snr;
    s_shared.neighbors = s_neighbors;
    xSemaphoreGive(s_state_mutex);

    if (idx >= 0) {
        ESP_LOGI(TAG, "Neighbor %08" PRIX32 " RSSI:%d SNR:%d (total: %d)%s", beacon.src_addr, rssi,
                 snr, neighbor_count(&s_neighbors), is_new_peer ? " [NEW]" : "");

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
                ESP_LOGW(TAG,
                         "SYBIL WARNING: beacon from %08" PRIX32
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
    if (!s_receipt_timer)
        return;

    uint32_t t = now_ms();
    uint32_t earliest_due = 0;
    bool have_pending = false;

    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used)
            continue;
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

static void queue_broadcast_delivery_receipt(uint32_t original_src_addr,
                                             uint32_t original_packet_id) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned (the receipt is
     * receipt_sign'd with the network key inside the builder). */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping delivery receipt");
        return;
    }

    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    size_t wire_len = 0;

    /* Determine receipt policy based on mesh size */
    uint8_t policy =
        mesh_broadcast_receipt_policy(0xFFFFFFFFu, (uint8_t)neighbor_count(&s_neighbors));
    uint8_t hop_limit = (policy >= 2) ? 8 : 1; /* full=8, neighbors-only=1 */

    /* ws 1.3b: draw the 48-bit origin seq once per receipt; the retry
     * queue below resends the SAME serialized bytes on loss (not a fresh
     * re-origination), so one seq draw per receipt is correct. Fail-closed:
     * no seq means no receipt goes out this round. */
    uint64_t receipt_seq;
    if (control_seq_next(&receipt_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping delivery receipt for pkt=%08" PRIX32,
                 original_packet_id);
        return;
    }

    esp_err_t err = mesh_build_broadcast_delivery_receipt_packet(
        s_identity->address, next_packet_id(), original_src_addr, original_packet_id, hop_limit,
        receipt_seq, buf, sizeof(buf), &wire_len);
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

    uint32_t slot_delay_ms = mesh_broadcast_receipt_slot_delay_ms(
        s_identity->address, original_packet_id, (uint8_t)neighbor_count(&s_neighbors));
    uint32_t initial_delay_ms = slot_delay_ms + (esp_random() % 400u); /* +0..399ms jitter */

    pending_receipt_t* item = &s_receipt_queue[slot];
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
        if (!s_receipt_queue[i].used)
            continue;
        if (s_receipt_queue[i].due_at_ms <= t_now) {
            due_idx = i;
            break;
        }
    }

    if (due_idx < 0) {
        mesh_schedule_next_receipt_timer();
        return;
    }

    pending_receipt_t* item = &s_receipt_queue[due_idx];
    uint8_t attempt_no = (uint8_t)(item->attempts_sent + 1u);

    /* TX path can block for CAD/LBT + radio wait; feed task WDT just before entering it. */
    esp_task_wdt_reset();

    int rc = mesh_tx(item->buf, item->wire_len, TX_KIND_RECEIPT);

    /* Deny behavior: receipts are deferred, not dropped. Reschedule with
     * exponential backoff (scaled by remaining receipt budget) so the
     * receipt can go out once tokens refill; drop only when all attempts
     * are exhausted. */
    if (rc == TX_GATE_ERR_BUDGET) {
        item->attempts_sent++;
        if (item->attempts_sent >= item->attempts_total) {
            ESP_LOGW(TAG,
                     "Delivery receipt DROPPED for pkt=%08" PRIX32
                     " (all %u attempts airtime-exhausted)",
                     item->original_packet_id, (unsigned)item->attempts_total);
            memset(item, 0, sizeof(*item));
        } else {
            uint32_t remaining = tx_gate_remaining(AIRTIME_TIER_RECEIPT);
            uint32_t scale_num = 1u;
            uint32_t scale_den = 1u;
            mesh_broadcast_receipt_retry_scale(remaining, &scale_num, &scale_den);
            if (!(scale_num == 1u && scale_den == 1u)) {
                uint32_t utilized_pct =
                    ((AIRTIME_BUDGET_RECEIPT_MS - (remaining > AIRTIME_BUDGET_RECEIPT_MS
                                                       ? AIRTIME_BUDGET_RECEIPT_MS
                                                       : remaining)) *
                     100u) /
                    AIRTIME_BUDGET_RECEIPT_MS;
                ESP_LOGD(TAG,
                         "Receipt retry multiplier=%" PRIu32 "/%" PRIu32 " (utilization=%" PRIu32
                         "%%)",
                         scale_num, scale_den, utilized_pct);
            }
            uint32_t raw_backoff_ms =
                1000u + ((uint32_t)item->attempts_sent * 2000u) + (esp_random() % 1000u);
            uint32_t backoff_ms = (raw_backoff_ms * scale_num) / scale_den;
            item->due_at_ms = t_now + backoff_ms;
            ESP_LOGW(TAG,
                     "Delivery receipt deferred for pkt=%08" PRIX32
                     " (attempt=%u/%u): airtime exhausted, retry in %" PRIu32 "ms",
                     item->original_packet_id, (unsigned)(item->attempts_sent),
                     (unsigned)item->attempts_total, backoff_ms);
        }
        mesh_schedule_next_receipt_timer();
        return;
    }

    if (rc == TX_GATE_OK) {
        ESP_LOGI(TAG,
                 "TX delivery receipt for broadcast pkt=%08" PRIX32 " to %08" PRIX32
                 " attempt=%u/%u",
                 item->original_packet_id, item->original_src_addr, (unsigned)attempt_no,
                 (unsigned)item->attempts_total);
    }

    item->attempts_sent++;
    if (item->attempts_sent >= item->attempts_total) {
        memset(item, 0, sizeof(*item));
        mesh_schedule_next_receipt_timer();
        return;
    }

    uint8_t i = (uint8_t)(item->attempts_sent - 1u);
    uint32_t remaining = tx_gate_remaining(AIRTIME_TIER_RECEIPT);
    uint32_t scale_num = 1u;
    uint32_t scale_den = 1u;
    mesh_broadcast_receipt_retry_scale(remaining, &scale_num, &scale_den);
    if (!(scale_num == 1u && scale_den == 1u)) {
        uint32_t utilized_pct =
            ((AIRTIME_BUDGET_RECEIPT_MS -
              (remaining > AIRTIME_BUDGET_RECEIPT_MS ? AIRTIME_BUDGET_RECEIPT_MS : remaining)) *
             100u) /
            AIRTIME_BUDGET_RECEIPT_MS;
        ESP_LOGD(TAG,
                 "Receipt retry multiplier=%" PRIu32 "/%" PRIu32 " (utilization=%" PRIu32 "%%)",
                 scale_num, scale_den, utilized_pct);
    }

    uint32_t raw_base_ms = 500u + ((uint32_t)i * 700u);
    uint32_t raw_jitter_range = 500u + ((uint32_t)i * 400u);
    uint32_t base_ms = (raw_base_ms * scale_num) / scale_den;
    uint32_t jitter_range = (raw_jitter_range * scale_num) / scale_den;
    if (jitter_range == 0u) {
        jitter_range = 1u;
    }
    uint32_t retry_delay_ms = base_ms + (esp_random() % jitter_range);
    item->due_at_ms = now_ms() + retry_delay_ms;

    mesh_schedule_next_receipt_timer();
}

static void mesh_receipt_timer_cb(void* arg) {
    (void)arg;
    if (!s_mesh_event_queue)
        return;

    mesh_event_type_t evt = MESH_EVT_RECEIPT_TX;
    if (xQueueSend(s_mesh_event_queue, &evt, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "mesh event queue full; dropped receipt timer event");
    }
}

static void mesh_schedule_next_probe_reply_timer(void) {
    if (!s_probe_reply_timer)
        return;

    uint32_t earliest_due = 0;
    if (!probe_reply_queue_earliest_due(s_probe_reply_queue, PROBE_REPLY_QUEUE_CAPACITY,
                                        &earliest_due)) {
        esp_timer_stop(s_probe_reply_timer);
        return;
    }

    uint32_t t = now_ms();
    uint32_t delay_ms = (earliest_due <= t) ? 1u : (earliest_due - t);
    esp_timer_stop(s_probe_reply_timer);
    esp_err_t err = esp_timer_start_once(s_probe_reply_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm probe reply timer: %d", (int)err);
    }
}

static void queue_probe_reply(const uint8_t* buf, uint8_t wire_len, uint32_t address) {
    uint32_t jitter_ms = esp_random() % 120u; /* +0..119, as before */
    uint32_t initial_delay_ms = probe_reply_initial_delay_ms(address, jitter_ms);
    uint32_t first_due_ms = probe_reply_attempt_due_ms(now_ms(), initial_delay_ms, 0);

    int slot = probe_reply_queue_insert(s_probe_reply_queue, PROBE_REPLY_QUEUE_CAPACITY, buf,
                                        wire_len, PROBE_REPLY_ATTEMPTS, first_due_ms);
    if (slot < 0) {
        ESP_LOGW(TAG, "Probe reply queue full; dropping reply");
        return;
    }
    mesh_schedule_next_probe_reply_timer();
}

static void mesh_process_probe_reply_tx_event(void) {
    uint32_t t_now = now_ms();
    int due_idx =
        probe_reply_queue_find_due(s_probe_reply_queue, PROBE_REPLY_QUEUE_CAPACITY, t_now);
    if (due_idx < 0) {
        mesh_schedule_next_probe_reply_timer();
        return;
    }

    pending_probe_reply_t* item = &s_probe_reply_queue[due_idx];

    /* TX can block for CAD/LBT; feed the task WDT just before entering it. */
    esp_task_wdt_reset();

    int rc = mesh_tx(item->buf, item->wire_len, TX_KIND_PROBE_REPLY);
    uint32_t t_after = now_ms(); /* measure the 140ms retry gap from TX return, matching the
                                    original vTaskDelay(140)-after-tx */

    /* Deny-stop vs. sent-and-retry decision lives in the pure state machine.
     * TX_GATE_ERR_BUDGET abandons the whole reply (first thing to shed);
     * otherwise the send is counted and the next attempt is scheduled
     * t_after + 140ms until attempts_total is reached. */
    probe_reply_tx_result_t result =
        (rc == TX_GATE_ERR_BUDGET) ? PROBE_REPLY_TX_DENIED : PROBE_REPLY_TX_SENT;
    probe_reply_queue_apply_result(item, result, t_after);

    mesh_schedule_next_probe_reply_timer();
}

static void mesh_probe_reply_timer_cb(void* arg) {
    (void)arg;
    if (!s_mesh_event_queue)
        return;

    mesh_event_type_t evt = MESH_EVT_PROBE_REPLY_TX;
    if (xQueueSend(s_mesh_event_queue, &evt, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "mesh event queue full; dropped probe reply timer event");
    }
}

static void send_ack(uint32_t dest_addr, uint32_t ack_packet_id, int8_t rssi) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned (ack_sign
     * needs the network key). The sender's retransmission timer covers the
     * missing ACK exactly like a lost one. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping ACK");
        return;
    }
    /* ws 1.3b: draw the 48-bit origin seq before building the struct, so it
     * can go straight into the designated initializer below instead of a
     * second pass. Fail-closed: no seq means no ACK goes out; the sender's
     * retransmission timer covers a missing ACK exactly like a lost one. */
    uint64_t ack_seq;
    if (control_seq_next(&ack_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping ACK for pkt=%08" PRIX32, ack_packet_id);
        return;
    }
    bramble_ack_t ack = {
        .header =
            {
                .version = BRAMBLE_VERSION,
                .type = PKT_TYPE_ACK,
                .flags = 0,
                /* Reactive: ROUTE_HOP_LIMIT_MAX (8). Flood transport: the
                 * flooded-ACK originates at the operator-settable flood hop
                 * budget so a confirmation can traverse the same diameter its
                 * DATA did. */
                .hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit),
                .dest_addr = dest_addr,
                .packet_id = next_packet_id(),
            },
        .src_addr = s_identity->address,
        .ack_packet_id = ack_packet_id,
        .ack_flags = 0,
        .rssi_at_dest = rssi,
        .hop_count = 1,
        .relay_path = {s_identity->address}, /* destination is first hop */
        .seq =
            {
                (uint8_t)(ack_seq >> 40),
                (uint8_t)(ack_seq >> 32),
                (uint8_t)(ack_seq >> 24),
                (uint8_t)(ack_seq >> 16),
                (uint8_t)(ack_seq >> 8),
                (uint8_t)ack_seq,
            },
    };
    /* NEW-SEC-8 (STAGED): sign after every field except relay_path/
     * hop_count/hop_limit is set (those are excluded from the MAC and
     * legitimately change per relay hop); seq is set above and IS covered
     * (ws 1.3b). */
    ack_sign(&ack);

    /* Red-team audit: was buf[64], a hand-counted constant. Not currently
     * truncating (send_ack always originates with hop_count 1), but
     * macro-ized to ACK_MAX_SIZE anyway so it can't silently start
     * truncating if that ever changes, matching forward_ack's fix below. */
    uint8_t buf[ACK_MAX_SIZE];
    esp_err_t err = bramble_ack_serialize(&ack, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ACK serialize failed");
        return;
    }
    size_t wire_len = bramble_ack_wire_size(&ack);
    /* Deny behavior: nothing to queue; the sender's retry scheduler covers
     * a lost ACK. CRITICAL can borrow from NORMAL, so denial here means
     * the node is severely over budget. */
    int ret = mesh_tx(buf, (uint8_t)wire_len, TX_KIND_ACK);
    if (ret == TX_GATE_OK) {
        ESP_LOGI(TAG, "ACK sent for pkt %08" PRIX32 " to %08" PRIX32 " (%u hops)", ack_packet_id,
                 dest_addr, ack.hop_count);
    }
}

static void forward_ack(bramble_ack_t* ack, int16_t rssi) {
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
    route_entry_t* route = route_lookup(&s_routes, ack->header.dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        ESP_LOGW(TAG, "No route to forward ACK to %08" PRIX32, ack->header.dest_addr);
        return;
    }

    /* Red-team fix: was buf[64], a hand-counted constant. ACK_MAX_SIZE (a
     * full 8-hop path) is 69 as of the ws 1.3b size bump, so a 7-hop (65B)
     * or 8-hop (69B) ACK overflowed this buffer and was silently dropped
     * (bramble_ack_serialize's len < need guard). */
    uint8_t buf[ACK_MAX_SIZE];
    esp_err_t err = bramble_ack_serialize(ack, buf, sizeof(buf));
    if (err != ESP_OK)
        return;

    size_t wire_len = bramble_ack_wire_size(ack);
    ESP_LOGI(TAG, "Forwarding ACK for pkt %08" PRIX32 " toward %08" PRIX32 " (%u hops)",
             ack->ack_packet_id, ack->header.dest_addr, ack->hop_count);
    mesh_tx(buf, (uint8_t)wire_len, TX_KIND_ACK);
}

static void handle_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_ack_t ack;
    esp_err_t err = bramble_ack_deserialize(&ack, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ACK deserialize failed");
        return;
    }

    /* NEW-SEC-8 (STAGED, see network_key.h): verify before ANY effect of
     * this ACK, on both branches below. A forged ACK must not cancel
     * retransmission, mark a message delivered, or be forwarded. */
    if (!ack_verify(&ack)) {
        ESP_LOGW(TAG, "ACK auth failed pkt=%08" PRIX32 " src=%08" PRIX32, ack.ack_packet_id,
                 ack.src_addr);
        return;
    }

    /* ws 1.3b: replay check on the authenticated signer (ack.src_addr is
     * MAC-covered, so an attacker cannot dodge the window by mutating it).
     * Checked after ack_verify and strictly before BOTH the forward branch
     * and the for-us effects below (pending_ack_remove, msg_store_update),
     * so a replayed ACK never cancels a live retransmission or forwards. */
    uint64_t ack_seq = ((uint64_t)ack.seq[0] << 40) | ((uint64_t)ack.seq[1] << 32) |
                       ((uint64_t)ack.seq[2] << 24) | ((uint64_t)ack.seq[3] << 16) |
                       ((uint64_t)ack.seq[4] << 8) | (uint64_t)ack.seq[5];
    if (!control_replay_ok(ack.src_addr, ack_seq)) {
        ESP_LOGW(TAG, "ACK replay pkt=%08" PRIX32 " src=%08" PRIX32, ack.ack_packet_id,
                 ack.src_addr);
        return;
    }

    /* Not for us — forward it */
    if (ack.header.dest_addr != s_identity->address) {
        if (s_flood_transport) {
            /* Flooding F1 Task 2: under s_flood_transport there are no routes,
             * so the ACK cannot be route-forwarded home. It FLOODS back
             * through the SAME engine the DATA flood uses (channel_flood_decide
             * + the jittered schedule_flood_relay queue + FLOOD_SUPPRESS_AFTER
             * suppression + airtime budget), authenticated: ack_verify above
             * already gated this branch, so a bad-MAC ACK was dropped before
             * ever reaching here and is never rebroadcast (the same "never act
             * on unauthenticated wire bytes" rule the DATA flood applies via
             * data_auth_verify). The dispatch s_dedup gate (packet_id ^ type)
             * already dedups the flooded ACK's own packet_id: copies 2+ never
             * reach handle_ack; they are counted at the dispatch gate for
             * suppression instead (see mesh_process_rx_packet). A re-ACK of a
             * duplicate DATA carries a FRESH header.packet_id (send_ack draws
             * next_packet_id every call), so it is not deduped and floods
             * anew, preserving the Phase 1 re-ACK-on-duplicate second chance.
             *
             * The flood dedup key mirrors the DATA flood's packet_id ^ src:
             * both fields are stable across relay hops (only relay_path/
             * hop_count/hop_limit are forward-mutated) and identify this ACK
             * for the suppression bookkeeping at the dispatch gate. A node that
             * hears its OWN originated ACK echoed back (ack.src_addr == self)
             * must not rebroadcast it, exactly like the DATA flood's
             * is_own_echo guard. */
            uint32_t ack_flood_key = ack.header.packet_id ^ ack.src_addr;
            bool is_own_echo = (ack.src_addr == s_identity->address);
            size_t cur_wire = bramble_ack_wire_size(&ack);
            bool budget_permits = tx_gate_check((uint8_t)cur_wire, TX_KIND_ACK);
            channel_flood_decision_t flood = channel_flood_decide(ack.header.hop_limit, is_own_echo,
                                                                  budget_permits, esp_random());
            if (flood.should_relay) {
                /* Append our address to the relay trail (relay_path/hop_count
                 * are MAC-excluded, mutated per hop exactly as forward_ack
                 * does) and decrement the hop limit to the flood engine's
                 * value, then re-serialize the mutated ACK for rebroadcast. */
                if (ack.hop_count < ACK_MAX_HOPS) {
                    ack.relay_path[ack.hop_count++] = s_identity->address;
                }
                ack.header.hop_limit = flood.new_hop_limit;
                uint8_t relay_buf[ACK_MAX_SIZE];
                if (bramble_ack_serialize(&ack, relay_buf, sizeof(relay_buf)) == ESP_OK) {
                    size_t wlen = bramble_ack_wire_size(&ack);
                    ESP_LOGI(TAG,
                             "Flooding ACK for pkt %08" PRIX32 " toward %08" PRIX32
                             " hop_limit->%u",
                             ack.ack_packet_id, ack.header.dest_addr, flood.new_hop_limit);
                    schedule_flood_relay(relay_buf, (uint8_t)wlen, flood.jitter_ms, ack_flood_key,
                                         TX_KIND_ACK);
                }
            } else if (!budget_permits) {
                ESP_LOGD(TAG, "Flooded ACK relay denied by airtime budget, pkt=%08" PRIX32,
                         ack.ack_packet_id);
            }
        } else {
            forward_ack(&ack, rssi);
        }
        return;
    }

    ESP_LOGI(TAG,
             "ACK received for pkt %08" PRIX32 " from %08" PRIX32 " (RSSI at dest: %d, %u hops)",
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
    if (msg_store_update_status_with_route(ack.ack_packet_id, MSG_STATUS_DELIVERED, route_hop_count,
                                           route_hops)) {
        record_ack_delivery_event(&ack);
        /* Notify webapp with full relay path from ACK */
        char addr_buf[12];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, ack.src_addr);
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "from", addr_buf);
        char pkt_buf[12];
        snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, ack.ack_packet_id);
        cJSON_AddStringToObject(params, "packet_id", pkt_buf);
        cJSON_AddStringToObject(params, "status", "delivered");
        cJSON_AddNumberToObject(params, "rssi_at_dest", ack.rssi_at_dest);

        cJSON* path = cJSON_AddArrayToObject(params, "relayPath");
        char hop_buf[12];
        for (uint8_t i = 0; i < route_hop_count; i++) {
            cJSON* hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, route_hops[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            cJSON_AddNumberToObject(hop, "rssi",
                                    (i == (route_hop_count - 1)) ? ack.rssi_at_dest : 0);
            cJSON_AddItemToArray(path, hop);
        }

        rpc_notify("bramble.onAck", params);
        cJSON_Delete(params);
    }

    if (found) {
        ESP_LOGI(TAG, "Message delivered to %08" PRIX32, ack.src_addr);
    }
}

static void forward_delivery_receipt(bramble_delivery_receipt_t* receipt) {
    if (!receipt)
        return;

    if (receipt->hop_count < DELIVERY_RECEIPT_MAX_HOPS) {
        receipt->relay_path[receipt->hop_count++] = s_identity->address;
    }

    if (receipt->header.hop_limit <= 1) {
        ESP_LOGD(TAG, "Delivery receipt hop limit reached, dropping");
        return;
    }
    receipt->header.hop_limit--;

    route_entry_t* route = route_lookup(&s_routes, receipt->header.dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        ESP_LOGW(TAG, "No route to forward delivery receipt to %08" PRIX32,
                 receipt->header.dest_addr);
        return;
    }

    /* Red-team audit: was buf[96], a hand-counted constant. Not currently
     * truncating (DELIVERY_RECEIPT_MAX_SIZE is 68 as of ws 1.3b), but
     * macro-ized for the same reason as the other TX buffers in this
     * file. */
    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    esp_err_t err = bramble_delivery_receipt_serialize(receipt, buf, sizeof(buf));
    if (err != ESP_OK)
        return;

    size_t wire_len = DELIVERY_RECEIPT_MIN_SIZE + ((size_t)receipt->hop_count * 4u);
    ESP_LOGI(TAG,
             "Forwarding delivery receipt for pkt %08" PRIX32 " toward %08" PRIX32 " (%u hops)",
             receipt->orig_packet_id, receipt->header.dest_addr, receipt->hop_count);
    /* Deny behavior: a forwarded receipt is best-effort on behalf of a
     * remote sender; suppress when the RECEIPT lane is exhausted. */
    if (mesh_tx(buf, (uint8_t)wire_len, TX_KIND_RECEIPT) == TX_GATE_ERR_BUDGET) {
        ESP_LOGW(TAG,
                 "Forwarded delivery receipt suppressed for pkt=%08" PRIX32
                 ": receipt airtime budget exhausted",
                 receipt->orig_packet_id);
    }
}

static void handle_delivery_receipt(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    (void)snr;
    bramble_delivery_receipt_t receipt;
    if (bramble_delivery_receipt_deserialize(&receipt, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Delivery receipt deserialize failed");
        return;
    }

    /* NEW-SEC-8 (STAGED, see network_key.h): verify before acting, on
     * both branches below. */
    if (!receipt_verify(&receipt)) {
        ESP_LOGW(TAG, "Delivery receipt auth failed pkt=%08" PRIX32 " src=%08" PRIX32,
                 receipt.orig_packet_id, receipt.src_addr);
        return;
    }

    /* ws 1.3b: replay check on the authenticated signer (receipt.src_addr
     * is MAC-covered, so an attacker cannot dodge the window by mutating
     * it). Checked after receipt_verify and strictly before BOTH the
     * forward branch and the for-us effect (the broadcast delivery
     * notification), so a replayed receipt never re-notifies or forwards. */
    uint64_t receipt_seq = ((uint64_t)receipt.seq[0] << 40) | ((uint64_t)receipt.seq[1] << 32) |
                           ((uint64_t)receipt.seq[2] << 24) | ((uint64_t)receipt.seq[3] << 16) |
                           ((uint64_t)receipt.seq[4] << 8) | (uint64_t)receipt.seq[5];
    if (!control_replay_ok(receipt.src_addr, receipt_seq)) {
        ESP_LOGW(TAG, "Delivery receipt replay pkt=%08" PRIX32 " src=%08" PRIX32,
                 receipt.orig_packet_id, receipt.src_addr);
        return;
    }

    if (receipt.header.dest_addr != s_identity->address) {
        forward_delivery_receipt(&receipt);
        return;
    }

    mesh_emit_broadcast_delivery_notification(receipt.src_addr, receipt.orig_packet_id, rssi,
                                              receipt.hop_count, receipt.relay_path);
}

/* Self-heal for a desynced DM session. handle_data calls this when it cannot
 * decrypt a directed DM from a peer: the peer's session has diverged from ours
 * (typically it rebooted and lost the session while we kept our stale one), so
 * every DM it sends would fail forever with no recovery. Re-initiate the
 * handshake so DMs recover on their own. Guards against abuse: only for a peer
 * we currently neighbor with, and at most once per interval per peer, so a
 * spray of undecryptable packets cannot be turned into a re-key / airtime DoS.
 * The DH-heavy INIT is queued to handshake_worker_task, never run on this (mesh
 * RX) task -- the same M7 rule process_ke_init/resp follow. */
#define DM_REHANDSHAKE_MIN_INTERVAL_MS 15000u
#define DM_REHANDSHAKE_TRACK 8
static struct {
    uint32_t addr;
    uint32_t last_ms;
} s_dm_rehs[DM_REHANDSHAKE_TRACK];

static bool dm_rehandshake_rate_ok(uint32_t peer, uint32_t now) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < DM_REHANDSHAKE_TRACK; i++) {
        if (s_dm_rehs[i].addr == peer) {
            if ((uint32_t)(now - s_dm_rehs[i].last_ms) < DM_REHANDSHAKE_MIN_INTERVAL_MS)
                return false;
            s_dm_rehs[i].last_ms = now;
            return true;
        }
        if (s_dm_rehs[i].addr == 0 && free_slot < 0)
            free_slot = i;
        if (s_dm_rehs[i].last_ms < s_dm_rehs[oldest].last_ms)
            oldest = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;
    s_dm_rehs[slot].addr = peer;
    s_dm_rehs[slot].last_ms = now;
    return true;
}

static void maybe_trigger_dm_rehandshake(uint32_t peer) {
    if (peer == 0 || peer == s_identity->address)
        return;
    if (!neighbor_lookup(&s_neighbors, peer))
        return; /* only real, currently-neighboring peers */
    if (!dm_rehandshake_rate_ok(peer, now_ms()))
        return;
    dm_handshake_work_item_t item;
    memset(&item, 0, sizeof(item));
    item.src_addr = peer;
    item.channel_idx = s_default_channel_idx;
    item.initiate = true;
    if (xQueueSend(s_handshake_work_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Rehandshake queue full, self-heal dropped for %08" PRIX32, peer);
        return;
    }
    ESP_LOGI(TAG, "DM session desync with %08" PRIX32 "; re-initiating handshake (self-heal)",
             peer);
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
        dm_session_t* sess = dm_lookup(&s_dm_table, src_addr);
        int ok = 0;
        if (sess && sess->state == DM_STATE_ACTIVE) {
            ok = (dm_session_decrypt(sess, &rx_hdr, src_addr, nonce, ciphertext, ct_len, tag,
                                     plaintext) == 0);
            if (ok)
                sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
        }
        xSemaphoreGive(s_dm_mutex);
        if (!ok) {
            ESP_LOGW(TAG, "Failed session decrypt from %08" PRIX32, src_addr);
            /* Our DM session with this peer has desynced (no session, or a stale
             * key it no longer holds). Silently returning here is what made DMs
             * fail permanently after one side rebooted; instead, kick a
             * rate-limited re-handshake so the session self-heals. */
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
        info.data_len = ct_len;
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
        if (rp == REPLAY_REJECT_DUP) {
            ESP_LOGD(TAG, "Replay drop from %08" PRIX32 " ctr=%llu", src_addr,
                     (unsigned long long)rx_counter);
            return;
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

            /* Validate fragment header — indices < total and total within limits */
            if (frag_hdr.frag_total > 1 && frag_hdr.frag_index < frag_hdr.frag_total &&
                frag_hdr.frag_total <= FRAG_MAX_FRAGMENTS) {
                /* This is a fragment — process through reassembly */
                ESP_LOGI(TAG, "RX fragment %u/%u msg_id=%04X from %08" PRIX32,
                         frag_hdr.frag_index + 1, frag_hdr.frag_total, frag_hdr.message_id,
                         info.src_addr);

                int ret =
                    reassembly_add(&s_reassembly, &frag_hdr, info.data + FRAG_HEADER_SIZE,
                                   info.data_len - FRAG_HEADER_SIZE, now_ms(), rx_hdr.packet_id);
                if (ret == 1) {
                    /* Reassembly complete — collect the full message.
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
                        int16_t channel_index = (int16_t)info.channel_id;
                        msg_store_add_ex2(info.src_addr, dir, text, tlen, rssi, snr, 0,
                                          MSG_STATUS_NONE, channel_index);

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
                                                    (info.channel_id > 0) ? info.channel_id : -1);
                            cJSON_AddBoolToObject(params, "broadcast",
                                                  (dir == MSG_DIR_BROADCAST_IN));
                            rpc_notify("bramble.onMessage", params);
                            cJSON_Delete(params);
                        }

                        /* Send ACK/receipt: use first-received fragment's packet_id for broadcasts
                         */
                        if (dir == MSG_DIR_INCOMING) {
                            send_ack(info.src_addr, rx_hdr.packet_id, rssi);
                            /* Task 6 (GAP A): record this delivery so a later
                             * duplicate of THIS fragment (same packet_id,
                             * the sender's retransmit after a lost ACK) is
                             * recognized at mesh_process_rx_packet's dedup
                             * gate and re-ACKed instead of silently dropped.
                             * Keyed like s_flood_dedup (packet_id ^
                             * src_addr). */
                            dedup_check_and_add(&s_delivered_dedup, rx_hdr.packet_id ^ src_addr,
                                                now_ms());
                        } else if (mesh_should_emit_broadcast_delivery_receipt(
                                       rx_hdr.dest_addr, (uint8_t)neighbor_count(&s_neighbors))) {
                            queue_broadcast_delivery_receipt(info.src_addr, first_frag_pkt_id);
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
        if (tlen >= sizeof(text))
            tlen = sizeof(text) - 1;
        memcpy(text, info.data, tlen);
        text[tlen] = '\0';

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "*** MESSAGE from %08" PRIX32 " ***", info.src_addr);
        ESP_LOGI(TAG, ">>> %s", text);
        ESP_LOGI(TAG, "*** (ch:%d RSSI:%d SNR:%d) ***", info.channel_id, rssi, snr);

        /* Store in message store — classify broadcast vs channel routing */
        uint32_t hdr_dest;
        memcpy(&hdr_dest, data + 4, 4); /* dest_addr at offset 4 in header */
        bool is_channel_message = (info.channel_id > 0);
        msg_direction_t dir = (hdr_dest == 0xFFFFFFFF && !is_channel_message) ? MSG_DIR_BROADCAST_IN
                                                                              : MSG_DIR_INCOMING;
        int16_t channel_index = (int16_t)info.channel_id;
        msg_store_add_ex2(info.src_addr, dir, text, tlen, rssi, snr, 0, MSG_STATUS_NONE,
                          channel_index);

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
            cJSON_AddNumberToObject(params, "channel",
                                    (info.channel_id > 0) ? info.channel_id : -1);
            cJSON_AddBoolToObject(params, "broadcast", dir == MSG_DIR_BROADCAST_IN);
            rpc_notify("bramble.onMessage", params);
            cJSON_Delete(params);
        }

        /* Send ACK for unicast messages (not broadcasts) */
        if (dir == MSG_DIR_INCOMING) {
            send_ack(info.src_addr, rx_hdr.packet_id, rssi);
            /* Task 6 (GAP A): record this delivery so a later duplicate
             * (same packet_id, the sender's retransmit after a lost ACK) is
             * recognized at mesh_process_rx_packet's dedup gate and
             * re-ACKed instead of silently dropped. Keyed like
             * s_flood_dedup (packet_id ^ src_addr). */
            dedup_check_and_add(&s_delivered_dedup, rx_hdr.packet_id ^ src_addr, now_ms());
        } else if (mesh_should_emit_broadcast_delivery_receipt(
                       rx_hdr.dest_addr, (uint8_t)neighbor_count(&s_neighbors))) {
            queue_broadcast_delivery_receipt(info.src_addr, rx_hdr.packet_id);
        }

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
 * Returns TX_GATE_OK, TX_GATE_ERR_BUDGET (denied, nothing transmitted)
 * or TX_GATE_ERR_RADIO. Per-kind deny behavior lives at the call sites.
 */
static int mesh_tx(const uint8_t* buf, uint8_t len, tx_kind_t kind) {
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

static void send_rreq(const bramble_rreq_t* rreq) {
    /* Red-team audit: was buf[64], a hand-counted constant. RREQ_SIZE (30)
     * is unaffected by the ws 1.3b size bumps and always fit, but
     * macro-ized for the same reason as the other TX buffers in this
     * file: a hand-counted constant can't warn you when it stops being
     * big enough. */
    uint8_t buf[RREQ_SIZE];
    if (bramble_rreq_serialize(rreq, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RREQ query=%08" PRIX32 " dest=%08" PRIX32, rreq->query_id,
                 rreq->header.dest_addr);
        /* Deny behavior: routing control rides the reserved CRITICAL lane
         * (can also borrow from NORMAL); if even that is exhausted the
         * discovery retry scheduler will try again. Log loudly. */
        if (mesh_tx(buf, HEADER_SIZE + 18, TX_KIND_ROUTING) == TX_GATE_ERR_BUDGET) {
            ESP_LOGW(TAG, "RREQ denied by airtime budget");
        }
    }
}

static void send_rrep(const bramble_rrep_t* rrep) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned. The RREP was
     * built and rrep_sign'd elsewhere; without the network key that MAC is the
     * all-zero sentinel, so do not transmit. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping RREP");
        return;
    }
    /* Red-team audit: was buf[64], a hand-counted constant. RREP_SIZE (40
     * as of ws 1.3b) always fit, but macro-ized for the same reason as
     * the other TX buffers in this file. */
    uint8_t buf[RREP_SIZE];
    if (bramble_rrep_serialize(rrep, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RREP query=%08" PRIX32 " → next=%08" PRIX32, rrep->query_id,
                 rrep->next_hop);
        /* Pre-existing bug fixed here (found while adding ws 1.3b's seq
         * field): this was HEADER_SIZE + 19 (31 bytes), 3 short of the
         * struct's 22-byte payload (query_id+src_addr+next_hop+hop_count+
         * route_metric+auth_hmac), truncating the last 3 bytes of
         * auth_hmac on every real transmit. RREP_SIZE (the macro, not a
         * hand-counted offset) is what every other RREP size check already
         * uses, so this can't drift again the way HEADER_SIZE+19 did. */
        if (mesh_tx(buf, RREP_SIZE, TX_KIND_ROUTING) == TX_GATE_ERR_BUDGET) {
            ESP_LOGW(TAG, "RREP denied by airtime budget");
        }
    }
}

static void send_rerr(uint32_t broken_dest, uint32_t broken_next_hop) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned (rerr_sign
     * needs the network key). */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping RERR");
        return;
    }
    /* components/routing/forwarding.c: rerr_build fills version/type/flags/
     * hop_limit/dest_addr/reporter_addr/broken_dest/broken_next_hop
     * identically to the struct literal this replaced. packet_id and seq
     * are this node's own counters (rerr_build leaves them zeroed since it
     * owns no sequencing state), so they're set here same as before. */
    bramble_rerr_t rerr = rerr_build(s_identity->address, broken_dest, broken_next_hop);
    rerr.header.packet_id = next_packet_id();
    /* ws 1.3b: every re-origination draws its own fresh seq (unlike RREP's
     * origin-stable seq, RERR's seq is per-hop, matching reporter_addr).
     * Fail-closed: no seq means no RERR goes out this call; the caller's
     * route-broken detection or forwarding chain simply doesn't propagate
     * this hop, rather than shipping an unfresh/replayable report. */
    uint64_t rerr_seq;
    if (control_seq_next(&rerr_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping RERR for broken_dest=%08" PRIX32,
                 broken_dest);
        return;
    }
    rerr.seq[0] = (uint8_t)(rerr_seq >> 40);
    rerr.seq[1] = (uint8_t)(rerr_seq >> 32);
    rerr.seq[2] = (uint8_t)(rerr_seq >> 24);
    rerr.seq[3] = (uint8_t)(rerr_seq >> 16);
    rerr.seq[4] = (uint8_t)(rerr_seq >> 8);
    rerr.seq[5] = (uint8_t)rerr_seq;
    /* SEC-H1 (STAGED): re-signed on every call, including re-origination,
     * since this function builds a fresh struct each time (fresh
     * reporter_addr/packet_id/seq), and reporter_addr/seq are now
     * MAC-covered alongside the origin-stable broken_dest/broken_next_hop
     * (ws 1.3b). */
    rerr_sign(&rerr);
    /* Red-team audit: was buf[64], a hand-counted constant. RERR_SIZE (38
     * as of ws 1.3b) always fit, but macro-ized for the same reason as
     * the other TX buffers in this file. */
    uint8_t buf[RERR_SIZE];
    if (bramble_rerr_serialize(&rerr, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RERR broken_dest=%08" PRIX32, broken_dest);
        /* RERR_SIZE (the macro), not a hand-counted offset: RREP's
         * equivalent hand-counted offset drifted 3 bytes short of its
         * struct for years before being caught in ws 1.3b Task 2. */
        if (mesh_tx(buf, RERR_SIZE, TX_KIND_ROUTING) == TX_GATE_ERR_BUDGET) {
            ESP_LOGW(TAG, "RERR denied by airtime budget");
        }
    }
}

/* ── Originator pseudonym helpers for RREQ privacy ─────────────────── */

/* No pseudonym lookup table exists: RREP correlation runs on query_ids in
 * the pending-discovery table, and every attempt (first try or retry) gets a
 * fresh query_id, so its pseudonym is re-derived on demand and never stored.
 * Retries being unlinkable new queries is the intended privacy behavior. */

/* ── End pseudonym helpers ───────────────────────────────────── */

/* ── Jittered RREQ forwarding (DES-3) ────────────────────────── */

/**
 * Queue an RREQ forward with random jitter so same-hop relays do not
 * rebroadcast at the same instant. Falls back to immediate transmission when
 * the queue is full (under a forward storm the jitter no longer matters, and
 * dropping the forward could sever the only path).
 */
static void schedule_rreq_forward(const bramble_rreq_t* fwd) {
    uint32_t jitter = discovery_forward_jitter_ms(esp_random());
    for (int i = 0; i < RREQ_FWD_QUEUE_CAPACITY; i++) {
        if (!s_rreq_fwd_queue[i].used) {
            s_rreq_fwd_queue[i].used = true;
            s_rreq_fwd_queue[i].due_at_ms = now_ms() + jitter;
            s_rreq_fwd_queue[i].rreq = *fwd;
            ESP_LOGD(TAG, "RREQ fwd query=%08" PRIX32 " jittered %" PRIu32 "ms", fwd->query_id,
                     jitter);
            return;
        }
    }
    ESP_LOGW(TAG, "RREQ fwd queue full; forwarding query=%08" PRIX32 " immediately", fwd->query_id);
    send_rreq(fwd);
}

/**
 * Transmit any due jittered RREQ forwards. Called from the mesh task main
 * loop, so forwards stay scheduled rather than blocking packet handling.
 */
static void process_rreq_forward_queue(uint32_t t) {
    for (int i = 0; i < RREQ_FWD_QUEUE_CAPACITY; i++) {
        if (s_rreq_fwd_queue[i].used && (int32_t)(t - s_rreq_fwd_queue[i].due_at_ms) >= 0) {
            send_rreq(&s_rreq_fwd_queue[i].rreq);
            s_rreq_fwd_queue[i].used = false;
        }
    }
}

/* ── End jittered RREQ forwarding ──────────────────────────────── */

/* ── Jittered channel-flood relay (Task 5) ──────────────────────── */

/**
 * Queue a broadcast/channel DATA rebroadcast with random jitter, exactly
 * like schedule_rreq_forward: same-hop relays that all decided to flood the
 * same frame should not key up at the same instant. Falls back to immediate
 * transmission when the queue is full (a forward storm makes the jitter
 * moot, and dropping the relay could be the only path further out).
 *
 * buf/len are the ALREADY relay-mutated wire bytes (hop_limit decremented,
 * prev_hop rewritten to this node -- see the caller in handle_data): this
 * function only owns the timing, not the frame content.
 *
 * flood_key = packet_id ^ src_addr (the caller already computed it for the
 * src-qualified flood dedup) is recorded on the queued entry so an overheard
 * duplicate of the SAME frame can find and suppress this pending relay before
 * it fires (Flooding F1; see channel_flood_note_overheard). heard starts at 0
 * -- the copy that triggered this schedule is the FIRST copy, never counted
 * as an overheard one.
 *
 * tx_kind is the airtime lane the relay is sent on: TX_KIND_DATA_BROADCAST
 * for a flooded DATA frame, TX_KIND_ACK for a flooded ACK (Flooding F1
 * Task 2). One queue + one suppression engine serves both; only the lane the
 * final mesh_tx debits differs.
 */
static void schedule_flood_relay(const uint8_t* buf, uint8_t len, uint32_t jitter_ms,
                                 uint32_t flood_key, tx_kind_t tx_kind) {
    for (int i = 0; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        if (!s_flood_relay_queue[i].used) {
            s_flood_relay_queue[i].used = true;
            s_flood_relay_queue[i].due_at_ms = now_ms() + jitter_ms;
            memcpy(s_flood_relay_queue[i].buf, buf, len);
            s_flood_relay_queue[i].len = len;
            s_flood_relay_queue[i].flood_key = flood_key;
            s_flood_relay_queue[i].heard = 0;
            s_flood_relay_queue[i].tx_kind = (uint8_t)tx_kind;
            ESP_LOGD(TAG, "Channel flood relay jittered %" PRIu32 "ms", jitter_ms);
            return;
        }
    }
    ESP_LOGW(TAG, "Flood relay queue full; relaying immediately");
    if (mesh_tx(buf, len, tx_kind) == TX_GATE_ERR_BUDGET) {
        ESP_LOGW(TAG, "Immediate flood relay denied by airtime budget");
    }
}

/**
 * Transmit any due jittered flood relays. Called from the mesh task main
 * loop alongside process_rreq_forward_queue, so relays stay scheduled
 * rather than blocking packet handling. The airtime budget gets the final,
 * authoritative say here (mesh_tx -> tx_gate_send): a node that was under
 * budget when it decided to relay but has since spent it (e.g. its own
 * traffic, or other jittered relays firing first) still yields instead of
 * transmitting -- the airtime-aware stop that keeps a saturated node from
 * amplifying a storm.
 */
static void process_flood_relay_queue(uint32_t t) {
    for (int i = 0; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        if (s_flood_relay_queue[i].used && (int32_t)(t - s_flood_relay_queue[i].due_at_ms) >= 0) {
            if (mesh_tx(s_flood_relay_queue[i].buf, s_flood_relay_queue[i].len,
                        (tx_kind_t)s_flood_relay_queue[i].tx_kind) == TX_GATE_ERR_BUDGET) {
                ESP_LOGD(TAG, "Jittered flood relay denied by airtime budget");
            }
            s_flood_relay_queue[i].used = false;
        }
    }
}

/* ── End jittered channel-flood relay ───────────────────────────── */

static void flush_queued_messages(uint32_t dest_addr) {
    /* Route-established trigger only. QUEUE_REASON_SESSION entries are
     * flushed separately by flush_session_queue on session establishment;
     * touching them here would re-run mesh_send_message's route+session
     * decision and double-queue them under a fresh slot. */
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (s_queued_msgs[i].used && s_queued_msgs[i].reason == QUEUE_REASON_ROUTE &&
            s_queued_msgs[i].dest_addr == dest_addr) {
            ESP_LOGI(TAG, "Sending queued msg to %08" PRIX32 " (%u bytes)", dest_addr,
                     (unsigned)s_queued_msgs[i].len);
            mesh_send_message(dest_addr, s_queued_msgs[i].data, s_queued_msgs[i].len);
            s_queued_msgs[i].used = false;
        }
    }
}

static void handle_rreq(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RREQ packet");
        return;
    }

    ESP_LOGI(TAG, "RX RREQ query=%08" PRIX32 " dest=%08" PRIX32 " hops=%u metric=%u", rreq.query_id,
             rreq.header.dest_addr, rreq.hop_count, rreq.metric);

    /* First-arrival dedup: the first flood copy wins. Path quality still
     * arbitrates at route_install time, between RREPs answering different
     * discovery attempts (each attempt floods under a fresh query_id). */
    if (rreq_dedup_check_and_add(&s_rreq_dedup, rreq.query_id, now_ms())) {
        ESP_LOGD(TAG, "Duplicate RREQ query=%08" PRIX32, rreq.query_id);
        return;
    }

    /* Record reverse route (for RREP path back) */
    reverse_route_add(&s_reverse_routes, rreq.query_id, rreq.prev_hop, now_ms());

    /* Is this RREQ for us? */
    if (rreq.header.dest_addr == s_identity->address) {
        ESP_LOGI(TAG, "RREQ is for us; sending RREP");
        bramble_rrep_t rrep = rrep_build_destination(&rreq, s_identity->address);

        /* ws 1.3b: draw the 48-bit origin seq and re-sign to cover it
         * (rrep_build_destination already signed once with seq=0 from the
         * zeroed struct; this re-sign is the one that ships). Fail-closed:
         * no seq means no RREP goes out this round, and the RREQ
         * originator's retry logic will try again later. */
        uint64_t rrep_seq;
        if (control_seq_next(&rrep_seq) != 0) {
            ESP_LOGE(TAG, "Seq counter unavailable, dropping RREP for query=%08" PRIX32,
                     rreq.query_id);
            return;
        }
        rrep.seq[0] = (uint8_t)(rrep_seq >> 40);
        rrep.seq[1] = (uint8_t)(rrep_seq >> 32);
        rrep.seq[2] = (uint8_t)(rrep_seq >> 24);
        rrep.seq[3] = (uint8_t)(rrep_seq >> 16);
        rrep.seq[4] = (uint8_t)(rrep_seq >> 8);
        rrep.seq[5] = (uint8_t)rrep_seq;
        rrep_sign(&rrep);

        /* Route RREP back toward the previous hop */
        reverse_route_t* rev = reverse_route_lookup(&s_reverse_routes, rreq.query_id);
        if (rev) {
            rrep.next_hop = rev->prev_hop;
        }
        send_rrep(&rrep);

        /* Install route to the source via prev_hop. The link penalty
         * subtracts from the higher-is-better path metric. */
        uint8_t metric = metric_apply_link_penalty(rreq.metric, (int8_t)rssi, snr);
        route_install(&s_routes, rreq.prev_hop, rreq.prev_hop, rreq.hop_count, metric, ROUTE_ACTIVE,
                      ROUTE_SRC_DISCOVERED, now_ms());
        return;
    }

    /* Not for us: check whether we already hold a fresh, trustworthy route
     * to the destination (Phase 2 "save reactive routing": intermediate-
     * node RREP; see discovery.h's rrep_build_intermediate/
     * intermediate_rrep_route_usable doc comments for the trust/freshness
     * rules). Answering here short-circuits discovery for this whole
     * subtree instead of needing the flood to reach D itself.
     *
     * Having replied, this node does NOT also forward the RREQ onward:
     * that is the airtime-saving half of the tradeoff (the point of this
     * feature is to cut RREQ flood cost, not just add RREP traffic on top
     * of an unchanged flood), and it is safe because the RREQ is a
     * broadcast: every OTHER neighbor that heard the same RREQ still makes
     * its own independent forward decision, so a subtree this node cannot
     * vouch for still gets flooded through other paths. If this node's
     * cached route turns out to be stale/wrong beyond what
     * intermediate_rrep_route_usable already guards against, the
     * originator's expanding-ring retry (a fresh query_id, see
     * discovery.h) still reaches D normally. */
    route_entry_t* cached_route = route_lookup(&s_routes, rreq.header.dest_addr);
    if (cached_route && intermediate_rrep_route_usable(cached_route, now_ms())) {
        ESP_LOGI(TAG, "Intermediate RREP for dest=%08" PRIX32 " via cached route (hops=%u)",
                 rreq.header.dest_addr, cached_route->hop_count);
        bramble_rrep_t rrep =
            rrep_build_intermediate(&rreq, cached_route, s_identity->address, (int8_t)rssi, snr);

        /* Same seq draw + re-sign convention as the "RREQ is for us"
         * branch above: this node is a fresh RREP signer (answering on D's
         * behalf), so it needs its own origin sequence number, not D's. */
        uint64_t rrep_seq;
        if (control_seq_next(&rrep_seq) != 0) {
            ESP_LOGE(TAG,
                     "Seq counter unavailable, dropping intermediate RREP for query=%08" PRIX32,
                     rreq.query_id);
            return;
        }
        rrep.seq[0] = (uint8_t)(rrep_seq >> 40);
        rrep.seq[1] = (uint8_t)(rrep_seq >> 32);
        rrep.seq[2] = (uint8_t)(rrep_seq >> 24);
        rrep.seq[3] = (uint8_t)(rrep_seq >> 16);
        rrep.seq[4] = (uint8_t)(rrep_seq >> 8);
        rrep.seq[5] = (uint8_t)rrep_seq;
        rrep_sign(&rrep);

        send_rrep(&rrep);

        /* Same as the "RREQ is for us" branch: install a route to the
         * RREQ's ultimate source via prev_hop, since this node is now
         * answering on D's behalf and should be just as reachable from the
         * source as the real destination would have been. */
        uint8_t src_metric = metric_apply_link_penalty(rreq.metric, (int8_t)rssi, snr);
        route_install(&s_routes, rreq.prev_hop, rreq.prev_hop, rreq.hop_count, src_metric,
                      ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, now_ms());
        return;
    }

    /* Not for us, and no usable cached route: schedule a jittered forward
     * while the hop budget lasts. The > 1 bound makes hop_limit N mean
     * N-hop reach exactly (a relay receiving 1 does not forward), matching
     * the spec and the simulator. */
    if (rreq.header.hop_limit > 1) {
        if (!rreq_fwd_allow(&s_rreq_fwd_rl, now_ms())) {
            ESP_LOGW(TAG, "Forwarded RREQ rate limited (query=%08" PRIX32 ")", rreq.query_id);
        } else {
            bramble_rreq_t fwd = rreq_forward(&rreq, s_identity->address, (int8_t)rssi, snr);
            schedule_rreq_forward(&fwd);
        }
    }
}

static void handle_rrep(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RREP packet");
        return;
    }

    /* SEC-H1 (STAGED, see network_key.h): reject before installing any
     * route from this RREP. Covers query_id/src_addr/hop_count/route_metric
     * only, so a legitimate relay's next_hop/header.dest_addr rewrite
     * (rrep_forward) still verifies. */
    if (!rrep_verify(&rrep)) {
        ESP_LOGW(TAG, "RREP auth failed query=%08" PRIX32 " src=%08" PRIX32, rrep.query_id,
                 rrep.src_addr);
        return;
    }

    ESP_LOGI(TAG, "RX RREP query=%08" PRIX32 " src=%08" PRIX32 " hops=%u", rrep.query_id,
             rrep.src_addr, rrep.hop_count);

    /* ws 1.3b: replay check on the authenticated signer (rrep.src_addr is
     * MAC-covered, so an attacker cannot dodge the window by mutating it).
     * Checked after rrep_verify and strictly before route_install, so a
     * replayed RREP never resurrects a stale route. */
    uint64_t rrep_seq = ((uint64_t)rrep.seq[0] << 40) | ((uint64_t)rrep.seq[1] << 32) |
                        ((uint64_t)rrep.seq[2] << 24) | ((uint64_t)rrep.seq[3] << 16) |
                        ((uint64_t)rrep.seq[4] << 8) | (uint64_t)rrep.seq[5];
    if (!control_replay_ok(rrep.src_addr, rrep_seq)) {
        ESP_LOGW(TAG, "RREP replay query=%08" PRIX32 " src=%08" PRIX32, rrep.query_id,
                 rrep.src_addr);
        return;
    }

    /* The route-install and deliver/forward/drop decision lives in
     * rrep_rx_decide, a pure host-testable function in components/routing.
     * The link penalty subtracts from the higher-is-better path metric. */
    uint8_t metric = metric_apply_link_penalty(rrep.route_metric, (int8_t)rssi, snr);
    rrep_rx_decision_t d =
        rrep_rx_decide(&rrep, s_identity->address, metric, &s_pending_disc, &s_reverse_routes);

    if (d.install_route) {
        route_install(&s_routes, d.route_dest, d.route_next_hop, d.route_hops, d.route_metric,
                      ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, now_ms());
    }

    switch (d.action) {
    case RREP_RX_DELIVER:
        /* This RREP is for us: we originated the RREQ. */
        ESP_LOGI(TAG, "Route discovered to %08" PRIX32 " (hops=%u, metric=%u)", d.deliver_dest,
                 d.route_hops, d.route_metric);
        discovery_remove(&s_pending_disc, d.deliver_dest);

        /* Flush queued messages waiting for this route */
        flush_queued_messages(d.deliver_dest);
        break;
    case RREP_RX_FORWARD: {
        /* Not for us: forward the RREP toward the originator via the reverse route. */
        bramble_rrep_t fwd = rrep_forward(&rrep, d.forward_to, s_identity->address);
        send_rrep(&fwd);
        break;
    }
    case RREP_RX_DROP:
    default:
        ESP_LOGW(TAG, "No reverse route for RREP query=%08" PRIX32, rrep.query_id);
        break;
    }
}

static void rerr_fastfail_notify(uint32_t packet_id, const char* reason, void* ctx) {
    (void)ctx;

    cJSON* params = cJSON_CreateObject();
    if (!params) {
        return;
    }

    char pkt_buf[12];
    snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, packet_id);
    cJSON_AddStringToObject(params, "packet_id", pkt_buf);
    cJSON_AddStringToObject(params, "status", "failed");
    if (reason) {
        cJSON_AddStringToObject(params, "reason", reason);
    }

    rpc_notify("bramble.onAck", params);
    cJSON_Delete(params);
}

static void handle_rerr(const uint8_t* data, uint8_t len) {
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RERR packet");
        return;
    }

    /* SEC-H1 (STAGED, see network_key.h): verify before ANY route
     * teardown. An unauthenticated RERR must never break a route: reject
     * before route_lookup/route_marked_broken/fastfail run at all. */
    if (!rerr_verify(&rerr)) {
        ESP_LOGW(TAG, "RERR auth failed dest=%08" PRIX32 " broken_hop=%08" PRIX32, rerr.broken_dest,
                 rerr.broken_next_hop);
        return;
    }

    ESP_LOGW(TAG, "RX RERR: dest=%08" PRIX32 " broken_hop=%08" PRIX32, rerr.broken_dest,
             rerr.broken_next_hop);

    /* ws 1.3b: replay check on the authenticated (reporter_addr, seq) pair
     * (both MAC-covered as of this change, so an attacker cannot dodge the
     * window by mutating either). Checked after rerr_verify and strictly
     * before any teardown effect (route_marked_broken, forwarding,
     * failfast), so a replayed RERR never re-tears-down a live route. */
    uint64_t rerr_seq = ((uint64_t)rerr.seq[0] << 40) | ((uint64_t)rerr.seq[1] << 32) |
                        ((uint64_t)rerr.seq[2] << 24) | ((uint64_t)rerr.seq[3] << 16) |
                        ((uint64_t)rerr.seq[4] << 8) | (uint64_t)rerr.seq[5];
    if (!control_replay_ok(rerr.reporter_addr, rerr_seq)) {
        ESP_LOGW(TAG, "RERR replay reporter=%08" PRIX32 " dest=%08" PRIX32, rerr.reporter_addr,
                 rerr.broken_dest);
        return;
    }

    /* Invalidate route if it uses the broken next hop. components/routing/
     * forwarding.c: rerr_handle does the route_lookup + state/fail_count
     * mutation (identical to the inline logic this replaced) and reports
     * back whether it actually marked a route broken, since only mesh_task
     * needs that to decide on re-origination and logging. */
    bool route_marked_broken = rerr_handle(&s_routes, &rerr);
    if (route_marked_broken) {
        ESP_LOGW(TAG, "Route to %08" PRIX32 " marked BROKEN", rerr.broken_dest);

        /* Forward RERR if hop limit allows */
        if (rerr.header.hop_limit > 1) {
            send_rerr(rerr.broken_dest, rerr.broken_next_hop);
        }
    }

    /* Fail fast for pending packets to the destination, even on forwarded RERRs */
    size_t failed = rerr_ack_failfast_for_dest(&s_pending_acks, rerr.broken_dest, "route_broken",
                                               rerr_fastfail_notify, NULL);
    if (failed > 0) {
        ESP_LOGW(TAG, "RERR fast-failed %u pending ACK(s) for dest %08" PRIX32 "%s",
                 (unsigned)failed, rerr.broken_dest,
                 route_marked_broken ? "" : " (forwarded RERR/no local next-hop match)");
    }
}

/* ── Data forwarding for multi-hop ──────────────────────────────────── */

/* ── Mailbox helpers ─────────────────────────────────────────────────── */

static bool mesh_mailbox_store(uint32_t src_addr, uint32_t dest_addr, const uint8_t* raw,
                               uint8_t raw_len, uint32_t packet_id) {
    if (!s_mailbox_enabled)
        return false;

    /* Component payload is capped at MAILBOX_MAX_PAYLOAD (200) bytes.
     * Raw packets that exceed this limit cannot be buffered — drop with a warning. */
    if (raw_len > MAILBOX_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Mailbox: packet too large to store (%u > %u bytes), dropping for %08" PRIX32,
                 raw_len, (unsigned)MAILBOX_MAX_PAYLOAD, dest_addr);
        return false;
    }

    int rc = mailbox_store(&s_mailbox, src_addr, dest_addr, raw, raw_len, packet_id, now_ms());
    if (rc == 0) {
        ESP_LOGI(TAG, "Mailbox: stored packet for %08" PRIX32 " (id=%08" PRIX32 ")", dest_addr,
                 packet_id);
        return true;
    } else if (rc == -2) {
        ESP_LOGD(TAG, "Mailbox: duplicate packet id=%08" PRIX32 ", not stored", packet_id);
    } else {
        ESP_LOGW(TAG, "Mailbox: store failed (rc=%d) for %08" PRIX32, rc, dest_addr);
    }
    return false;
}

static void mailbox_flush_for(uint32_t dest_addr) {
    mailbox_entry_t entries[MAILBOX_MAX_PER_DEST];
    int count = mailbox_retrieve(&s_mailbox, dest_addr, entries, MAILBOX_MAX_PER_DEST);
    for (int i = 0; i < count; i++) {
        /* Wire v4: mailbox entries are raw DATA packet bytes captured at
         * store time (mesh_mailbox_store, forward_data_packet's no-route
         * branch), so their prev_hop byte range reflects whoever wrote it
         * back then, not us. WE are the transmitter on this flush, so
         * rewrite prev_hop to our own address before TX, exactly like
         * forward_data_packet's rewrite -- otherwise the recipient would
         * learn a stale/wrong reverse-route hop from a store-and-forward
         * delivery. */
        if (entries[i].payload_len >= BRAMBLE_DATA_PREV_HOP_OFFSET + 4) {
            memcpy(entries[i].payload + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
        }
        ESP_LOGI(TAG,
                 "Mailbox: delivering stored packet to %08" PRIX32 " (id=%08" PRIX32 " len=%u)",
                 dest_addr, entries[i].packet_id, entries[i].payload_len);
        /* Deny behavior: budget denial and radio failure both re-store the
         * entry for the next flush; stored mail is never silently lost. */
        int rc = mesh_tx(entries[i].payload, (uint8_t)entries[i].payload_len, TX_KIND_MAILBOX);
        if (rc != 0) {
            /* Transmit failed (LBT / radio busy) — re-store for retry on next flush */
            ESP_LOGW(TAG, "Mailbox: transmit failed (rc=%d) for id=%08" PRIX32 ", re-queuing", rc,
                     entries[i].packet_id);
            mailbox_store(&s_mailbox, entries[i].src_addr, entries[i].dest_addr, entries[i].payload,
                          entries[i].payload_len, entries[i].packet_id, entries[i].stored_at_ms);
        }
    }
}

static void mailbox_expire(uint32_t t) { mailbox_purge_expired(&s_mailbox, t); }

static void forward_data_packet(const uint8_t* data, uint8_t len, const bramble_header_t* header) {
    /* components/routing/forwarding.c: forward_data() owns the route-lookup
     * plus hop-limit-decrement decision (the same function gosim's bridge.c
     * already calls, and test_forwarding.c already exercises). Task 2 (ws
     * 1.4): mesh_task keeps only its own side effects around the decision:
     * mailbox-store-on-no-route, RERR-on-no-route, the actual TX, and stats.
     * Two behavioral deltas came along for the ride, both resolved by
     * adopting the tested/shipped-by-gosim behavior rather than silently
     * keeping the untested one (see task-2-report.md for the full list):
     *   - a STALE route used to forward is now promoted to ACTIVE with a
     *     refreshed last_confirmed (forward_data_packet never did this);
     *   - route last_used/use_count are now bumped at decision time
     *     (inside forward_data()) rather than only after a successful
     *     mesh_tx, so a budget-denied forward still counts as "used". */
    uint8_t hop_limit = header->hop_limit;
    forward_result_t fwd = forward_data(&s_routes, header->dest_addr, &hop_limit, now_ms());

    if (!fwd.should_send) {
        if (!fwd.route_error) {
            /* Hop limit already exhausted: silent drop, no mailbox/RERR,
             * matching the pre-refactor behavior exactly. */
            ESP_LOGD(TAG, "Data packet hop limit reached, dropping");
            return;
        }

        /* No usable route (unknown dest or ROUTE_BROKEN). */
        /* Extract src_addr from the data packet body (offset
         * BRAMBLE_DATA_SRC_ADDR_OFFSET). */
        uint32_t fwd_src_addr = 0;
        if (len >= BRAMBLE_DATA_SRC_ADDR_OFFSET + 4) {
            memcpy(&fwd_src_addr, data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
        }
        /* If mailbox enabled, store for later delivery instead of dropping */
        if (s_mailbox_enabled &&
            mesh_mailbox_store(fwd_src_addr, header->dest_addr, data, len, header->packet_id)) {
            ESP_LOGI(TAG, "No route to %08" PRIX32 ": stored in mailbox", header->dest_addr);
        } else {
            ESP_LOGW(TAG, "No route to forward data for %08" PRIX32, header->dest_addr);
            send_rerr(header->dest_addr, s_identity->address);
        }
        return;
    }

    /* Rebuild header with the hop limit forward_data() already decremented */
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    memcpy(buf, data, len);

    bramble_header_t fwd_hdr = *header;
    fwd_hdr.hop_limit = hop_limit;
    bramble_header_serialize(&fwd_hdr, buf, HEADER_SIZE);

    /* Wire v4: overwrite prev_hop with OUR OWN address before rebroadcast,
     * mirroring RREP's forwarder-address rewrite (#119). This is what lets
     * the next hop learn a route back to this DATA's originator via US,
     * closing the reverse-route gap that made multi-hop delivery
     * confirmations die at the first relay. Relay-mutable/MAC-excluded, so
     * this rewrite never touches anything under the AEAD tag. */
    if (len >= BRAMBLE_DATA_PREV_HOP_OFFSET + 4) {
        memcpy(buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    }

    ESP_LOGI(TAG, "Forwarding data to %08" PRIX32 " via %08" PRIX32, header->dest_addr,
             fwd.next_hop);
    /* Deny behavior: relayed traffic is dropped when the NORMAL lane is
     * exhausted; the originator's ACK-driven retries cover recovery. */
    if (mesh_tx(buf, len, TX_KIND_FORWARD) == TX_GATE_ERR_BUDGET) {
        ESP_LOGW(TAG, "Forward denied by airtime budget for %08" PRIX32, header->dest_addr);
    }
}

/*
 * Per-node identity Phase 3 (Part B): receive, pin, and flood-relay an
 * identity attestation. Verification ORDER is the security design:
 *
 *   1. exact-length deserialize;
 *   2. ident_relay_verify: the CHEAP network-key MAC, checked before
 *      anything else. Fail = drop: no relay, no pinning, no Ed25519
 *      verify ever runs. Keyless frames die at the first hop, so an
 *      outsider can neither get spam flooded nor grind this node's CPU
 *      with Ed25519 verifies;
 *   3. control_replay_ok on (src_addr, seq), both MAC-covered, so a
 *      captured attestation cannot be re-injected (packet_id is NOT
 *      MAC-covered, so the dispatch s_dedup gate alone would not stop a
 *      replay with a rewritten packet_id; this does);
 *   4. flood dedup (s_flood_dedup, packet_id ^ src_addr, the same
 *      src-qualified key the DATA flood uses);
 *   5. DELIVER to the identity module regardless of the relay decision:
 *      identity_store_handle_attestation runs the one receive-side
 *      Ed25519 verify and TOFU-pins (see identity_store.h);
 *   6. RELAY exactly like the broadcast DATA flood: channel_flood_decide
 *      (hop-limit floor, duplicate/own-echo suppression, airtime budget)
 *      + the shared jittered schedule_flood_relay queue. The frame
 *      rebroadcasts UNMODIFIED except the hop_limit decrement (the MAC
 *      excludes the header, so pass-through is valid; seq is never
 *      re-drawn by relays).
 *
 * Residual (accepted): relays do NOT Ed25519-verify, so a MAC-valid
 * frame with a garbage sig (a keyed insider misbehaving) still floods,
 * bounded by the airtime budget; every RECEIVER rejects it at the
 * Ed25519 check and counts it (identity_store's sig_failures).
 */
static void handle_identity_attestation(const uint8_t* data, uint8_t len) {
    bramble_identity_attestation_t att;
    if (bramble_identity_attestation_deserialize(&att, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid identity attestation (len=%u)", len);
        return;
    }

    if (!ident_relay_verify(&att)) {
        ESP_LOGW(TAG, "Identity attestation auth failed src=%08" PRIX32 ", drop", att.src_addr);
        return;
    }

    uint64_t att_seq = ((uint64_t)att.seq[0] << 40) | ((uint64_t)att.seq[1] << 32) |
                       ((uint64_t)att.seq[2] << 24) | ((uint64_t)att.seq[3] << 16) |
                       ((uint64_t)att.seq[4] << 8) | (uint64_t)att.seq[5];
    if (!control_replay_ok(att.src_addr, att_seq)) {
        ESP_LOGW(TAG, "Identity attestation replay src=%08" PRIX32, att.src_addr);
        return;
    }

    uint32_t flood_key = att.header.packet_id ^ att.src_addr;
    bool is_dup = dedup_check_and_add(&s_flood_dedup, flood_key, now_ms());

    /* Trust-anchor campaign (P2): the wall-clock epoch for the endorsement
     * expiry check. Use network time ONLY when timesync is confident (the same
     * fail-closed gate handle_data uses for deferred replay); otherwise pass 0
     * so the store does not enforce expiry against an untrusted clock. v1 certs
     * are permanent (UINT64_MAX) so this never fires live, but the pin gate is
     * ready for expiring certs the frozen wire format allows. */
    int64_t net_time_ms = timesync_get_network_time(&s_timesync, now_ms());
    /* Only pass a positive, confident epoch; otherwise 0 (the "unsynced"
     * sentinel the store treats as "do not enforce expiry"). The > 0 guard
     * stops a non-positive int64 from casting to a huge uint64 that would
     * spuriously expire a future non-permanent cert. */
    uint64_t epoch_ms = (timesync_is_confident(&s_timesync, now_ms()) && net_time_ms > 0)
                            ? (uint64_t)net_time_ms
                            : 0;

    /* Deliver locally regardless of the relay decision below. */
    identity_pin_result_t pin = identity_store_handle_attestation(
        &s_identity_pins, &att, s_identity->address, now_ms(), epoch_ms);
    switch (pin) {
    case IDENTITY_PIN_NEW:
        ESP_LOGI(TAG, "Identity pinned: %08" PRIX32 " (%d pinned)", att.src_addr,
                 identity_store_count(&s_identity_pins));
        break;
    case IDENTITY_PIN_CONFLICT:
        /* Impersonation detected: a network-key holder attested this
         * address under DIFFERENT keys than the pinned binding. First
         * seen wins; the original binding survives. */
        ESP_LOGW(TAG,
                 "IDENTITY CONFLICT for %08" PRIX32 ": attestation with different keys REFUSED"
                 " (conflicts=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.conflicts);
        break;
    case IDENTITY_PIN_BAD_SIG:
        ESP_LOGW(TAG,
                 "Identity attestation Ed25519 sig invalid src=%08" PRIX32 " (keyed garbage,"
                 " sig_failures=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.sig_failures);
        break;
    case IDENTITY_PIN_ADDR_MISMATCH:
        /* Phase 4 address<->key binding: a keyed member attested an
         * address its own Ed25519 key does not derive to. Impersonation
         * attempt (or a badly broken sender), refused on first contact. */
        ESP_LOGW(TAG,
                 "IDENTITY ADDR MISMATCH: %08" PRIX32 " claimed without the deriving key,"
                 " REFUSED (addr_mismatches=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.addr_mismatches);
        break;
    case IDENTITY_PIN_UNENDORSED:
        /* Trust-anchor gate (P2): this node is anchored and the attestation
         * carried no cert (or one not signed by our anchor for this key).
         * NOT pinned; the frame was still relayed (endorsement gates pinning
         * only, never liveness). */
        ESP_LOGW(TAG,
                 "Identity attestation UNENDORSED src=%08" PRIX32 " (no valid anchor cert,"
                 " unendorsed=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.unendorsed);
        break;
    case IDENTITY_PIN_EXPIRED:
        /* Anchored + a valid but EXPIRED cert (not_after past the synced
         * clock). v1 certs are permanent so this is not expected live. */
        ESP_LOGW(TAG, "Identity attestation EXPIRED cert src=%08" PRIX32 " (expired=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.expired);
        break;
    case IDENTITY_PIN_REFRESHED:
    case IDENTITY_PIN_SELF:
    default:
        break;
    }

    /* M2 TOFU-session teardown (identity-campaign follow-up): whenever this
     * attestation left a TRUSTED pinned binding for att.src_addr (NEW,
     * REFRESHED, or CONFLICT - the first-seen binding survives a CONFLICT and
     * is authoritative), drop any ESTABLISHED DM session whose cached peer
     * X25519 key disagrees with that pin. Such a session was a first-contact
     * TOFU handshake pointed at an impostor: the attestation is self-signed,
     * address-bound, and on an anchored node ALSO anchor-endorsed, so the pin
     * is authoritative and the stale session is dropped (recovered by a fresh,
     * now pin-continuity-checked handshake). This closes "a TOFU DM with a
     * Sybil that never endorses gets torn down the instant the real endorsed
     * peer pins." Fail-safe: it only ever DROPS; it never touches a
     * key-MATCHING (healthy) session or a non-ACTIVE handshaking slot. The
     * lookup+compare+teardown run as ONE critical section under s_dm_mutex
     * because process_ke_init/resp on the handshake worker mutate the same
     * slot; logging is deferred until after the lock is released, matching the
     * s_dm_mutex convention elsewhere in this file. */
    if (pin == IDENTITY_PIN_NEW || pin == IDENTITY_PIN_REFRESHED || pin == IDENTITY_PIN_CONFLICT) {
        const identity_pin_t* pinned = identity_store_lookup(&s_identity_pins, att.src_addr);
        if (pinned) {
            bool torn_down = false;
            xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
            dm_session_t* sess = dm_lookup(&s_dm_table, att.src_addr);
            if (sess && dm_pin_disagrees(sess, pinned->x25519_pub)) {
                dm_session_teardown(&s_dm_table, att.src_addr);
                torn_down = true;
            }
            xSemaphoreGive(s_dm_mutex);
            if (torn_down)
                ESP_LOGW(TAG,
                         "DM session torn down: pinned identity for %08" PRIX32
                         " disagrees with the TOFU session key",
                         att.src_addr);
        }
    }

    /* Relay through the SAME engine as the broadcast DATA flood
     * (handle_data): channel_flood_decide + schedule_flood_relay, on the
     * BROADCAST budget lane. Own-echo folds into is_duplicate exactly like
     * the DATA flood's is_own_echo. */
    bool is_own_echo = (att.src_addr == s_identity->address);
    bool budget_permits = tx_gate_check(len, TX_KIND_DATA_BROADCAST);
    channel_flood_decision_t flood = channel_flood_decide(
        att.header.hop_limit, is_dup || is_own_echo, budget_permits, esp_random());
    if (flood.should_relay) {
        uint8_t relay_buf[IDENTITY_ATTESTATION_SIZE];
        memcpy(relay_buf, data, len);
        bramble_header_t relay_hdr = att.header;
        relay_hdr.hop_limit = flood.new_hop_limit;
        bramble_header_serialize(&relay_hdr, relay_buf, HEADER_SIZE);
        ESP_LOGI(TAG, "Identity attestation relay from %08" PRIX32 " hop_limit %u->%u",
                 att.src_addr, att.header.hop_limit, flood.new_hop_limit);
        schedule_flood_relay(relay_buf, len, flood.jitter_ms, flood_key, TX_KIND_DATA_BROADCAST);
    } else if (!budget_permits) {
        ESP_LOGD(TAG, "Identity attestation relay denied by airtime budget, src=%08" PRIX32,
                 att.src_addr);
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

    /* Record raw RX event */
    traffic_debug_record_rx(&s_traffic_debug, header.type, pkt->len, pkt->rssi);

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
 * Compute adaptive beacon interval based on current mesh conditions.
 * Returns new interval in milliseconds.
 */
static uint32_t compute_adaptive_beacon_interval(uint32_t t, uint8_t neighbor_count) {
    uint8_t churn =
        beacon_churn_count(s_churn_history, MAX_CHURN_HISTORY, t, s_beacon_policy.churn_window_ms);

    beacon_interval_decision_t d = beacon_interval_decide(
        s_beacon_policy.enabled, s_beacon_policy.mode == BEACON_MODE_ADAPTIVE,
        s_beacon_policy.base_interval_ms, s_beacon_policy.min_interval_ms,
        s_beacon_policy.max_interval_ms, s_beacon_policy.dense_threshold,
        s_beacon_policy.churn_threshold, neighbor_count, churn);

    if (!d.adaptive_active) {
        s_beacon_status.in_backoff = false;
        return d.interval_ms;
    }

    s_beacon_status.churn_events = churn;
    s_beacon_status.neighbor_count = neighbor_count;

    beacon_policy_mode_t prev_mode = s_beacon_status.active_mode;
    s_beacon_status.active_mode = BEACON_MODE_ADAPTIVE;
    s_beacon_status.in_backoff = d.in_backoff ? true : false;
    s_beacon_status.current_interval_ms = d.interval_ms;

    if (prev_mode != s_beacon_status.active_mode ||
        (s_beacon_status.last_transition_ms == 0 && s_beacon_policy.enabled)) {
        s_beacon_status.last_transition_ms = t;
        ESP_LOGI(TAG, "Beacon policy: neighbors=%u churn=%u interval=%lums %s", neighbor_count,
                 churn, (unsigned long)d.interval_ms,
                 d.in_backoff
                     ? "DENSE"
                     : (d.interval_ms < s_beacon_policy.base_interval_ms ? "CHURN" : "STABLE"));
    }

    return d.interval_ms;
}

int mesh_set_beacon_policy(const beacon_policy_config_t* config) {
    if (!config)
        return -1;

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
    if (nvs_open(NVS_NS_BACKPRESSURE, NVS_READWRITE, &nvs) != ESP_OK) {
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

    ESP_LOGI(TAG, "Beacon policy updated: enabled=%d mode=%d base=%lums", config->enabled,
             config->mode, (unsigned long)config->base_interval_ms);
    return 0;
}

void mesh_get_beacon_policy(beacon_policy_config_t* config) {
    if (!config)
        return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *config = s_beacon_policy;
    xSemaphoreGive(s_state_mutex);
}

void mesh_get_beacon_status(beacon_policy_status_t* status) {
    if (!status)
        return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *status = s_beacon_status;
    xSemaphoreGive(s_state_mutex);
}

void mesh_beacon_policy_load_config(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BACKPRESSURE, NVS_READONLY, &nvs) != ESP_OK) {
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

    ESP_LOGI(TAG, "Loaded beacon policy: enabled=%d mode=%d base=%lums", enabled, mode,
             (unsigned long)base_ms);
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
#define PROBE_SWEEP_ROUNDS 3
#define PROBE_SWEEP_INTERVAL_MS 350
#define PROBE_COLLECTION_WINDOW_MS 5000

static uint32_t s_probe_id;
static uint32_t s_probe_sent_ms;
static bool s_probe_collecting;
static bool s_probe_complete_emitted;
static probe_result_t s_probe_results[MAX_PROBE_RESULTS];
static int s_probe_result_count;
static uint8_t s_probe_rounds_sent;
static uint32_t s_probe_next_round_ms;
static bool s_probe_request_pending;
static uint32_t s_probe_request_id;

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

        /* Add jitter for next interval */
        uint8_t j[2];
        crypto_random(j, 2);
        int32_t jitter =
            ((int32_t)(j[0] | (j[1] << 8)) % (BEACON_JITTER_MS * 2)) - BEACON_JITTER_MS;
        *beacon_interval = base_interval + jitter;
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
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.neighbors = s_neighbors;
        xSemaphoreGive(s_state_mutex);

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
                s_queued_msgs[i].used = false;
            }
        }
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

    /* ACK retry tick — retransmit unacknowledged packets */
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

                    msg_store_update_status(pa->packet_id, MSG_STATUS_FAILED);
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

    /* Post-boot identity attestation (Phase 2): radio is up and the beacon
     * jitter already spread us out; announce {addr, X25519, Ed25519} once
     * now, then every ATTESTATION_INTERVAL_MS via periodic maintenance. */
    attempt_identity_attestation(now_ms());

    ESP_LOGI(TAG, "=== BOOT STAGE: entering main mesh loop ===");

    /* Main loop */
    while (1) {
        uint32_t t = now_ms();

        /* Reset task watchdog — if this stops being called, WDT resets device */
        esp_task_wdt_reset();

        /* Check if radio was hard-reset and needs reconfiguration */
        if (radio_check_and_clear_reinit()) {
            ESP_LOGW(TAG, "Radio recovered from stuck BUSY — resuming RX");
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
static uint32_t send_data_packet(uint32_t dest_addr, const uint8_t* payload, size_t payload_len,
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

/*
 * SEC-C2 / Task 1.4: sends a chat payload under an ESTABLISHED session key
 * (dm_session_encrypt), FLAG_ENCRYPT WITHOUT FLAG_CHANNEL. This is the DM
 * PAYLOAD path; it never falls back to the channel key. Caller MUST already
 * hold s_dm_mutex (this function reads/writes *sess, which lives inside
 * s_dm_table) and must have already checked sess->state == DM_STATE_ACTIVE.
 * Mirrors send_data_packet's nonce/TX/pending_ack handling exactly, minus
 * the channel_msg framing (a session is 1:1, so there is no channel_id/
 * epoch/app_type to multiplex; the wire layout is header+src_addr+nonce+
 * ciphertext+tag same as the channel path, just under a different key and
 * without FLAG_CHANNEL, which is exactly the signal handle_data uses to
 * pick the decrypt path on the other end).
 */
static uint32_t send_dm_packet(uint32_t dest_addr, const uint8_t* payload, size_t payload_len,
                               dm_session_t* sess) {
    uint8_t ciphertext[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    size_t total =
        BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + payload_len + BRAMBLE_TAG_SIZE;
    if (total > 255) {
        ESP_LOGE(TAG, "DM packet too large: %u bytes", (unsigned)total);
        return 0;
    }

    uint8_t buf[255];
    uint32_t pkt_id = next_packet_id();
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_DATA,
        .flags = FLAG_ENCRYPT, /* no FLAG_CHANNEL: session-keyed DM (SEC-C2) */
        .hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit),
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };

    bramble_header_serialize(&header, buf, HEADER_SIZE);

    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    xSemaphoreGive(s_nonce_mutex);
    if (nonce_ret != 0) {
        ESP_LOGE(TAG, "Nonce counter unavailable, dropping DM send: %d", nonce_ret);
        return 0;
    }

    if (dm_session_encrypt(sess, &header, s_identity->address, payload, payload_len, nonce,
                           ciphertext, tag) != 0) {
        ESP_LOGE(TAG, "Session encrypt failed for %08" PRIX32, dest_addr);
        return 0;
    }

    memcpy(buf + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
    /* Wire v4: ORIGINATOR writes its own address as prev_hop; see
     * send_data_packet's identical comment. */
    memcpy(buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    /* Wire v4 (F1): origin-authenticate; see send_data_packet. Mandatory-
     * provisioning (Task 2): abort if unprovisioned. */
    if (data_auth_sign(&header, s_identity->address, buf + BRAMBLE_DATA_AUTH_HMAC_OFFSET) != 0) {
        ESP_LOGD(TAG, "unprovisioned: inert, dropping DM send");
        return 0;
    }
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext, payload_len);
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + payload_len, tag,
           BRAMBLE_TAG_SIZE);

    int ret = mesh_tx(buf, (uint8_t)total, TX_KIND_DATA);
    if (ret == TX_GATE_OK) {
        pending_ack_add(&s_pending_acks, pkt_id, dest_addr, MSG_TIER_NORMAL, buf, (uint16_t)total,
                        now_ms());
        sess->msg_count++;
        sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
        return pkt_id;
    }
    return 0;
}

/*
 * SEC-C2 / Task 1.4: sends a handshake message (INIT or RESP) as an
 * APP_TYPE_KE inner payload of a DATA envelope under the CHANNEL key. This
 * is the handshake TRANSPORT (no session exists yet by definition), which
 * is why it reuses send_data_packet unmodified rather than send_dm_packet.
 */
static uint32_t send_ke_envelope(uint32_t dest_addr, int channel_idx,
                                 const bramble_key_exchange_t* ke) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGE(TAG, "send_ke_envelope: invalid channel index %d", channel_idx);
        return 0;
    }
    uint8_t wire[KEY_EXCHANGE_SIZE];
    if (bramble_key_exchange_serialize(ke, wire, sizeof(wire)) != ESP_OK) {
        ESP_LOGE(TAG, "KE envelope serialize failed");
        return 0;
    }
    return send_data_packet(dest_addr, wire, sizeof(wire), &s_channels[channel_idx], APP_TYPE_KE);
}

/* Public identity-key caching heuristic: peer_id_pub is a public value (sent
 * in the clear as long_term_pubkey on every handshake message), so a plain
 * early-exit scan leaks nothing secret; it is not a tag/key comparison.
 * dm_alloc memsets a fresh or evicted slot, so all-zero reliably means "no
 * identity cached here yet" for any slot this node has allocated. */
static int dm_session_has_peer_id(const dm_session_t* s) {
    if (!s)
        return 0;
    for (int i = 0; i < 32; i++) {
        if (s->peer_id_pub[i] != 0)
            return 1;
    }
    return 0;
}

static void pending_eph_store(uint32_t peer_addr, const uint8_t eph_priv[32],
                              const uint8_t eph_pub[32]) {
    int free_idx = -1;
    for (int i = 0; i < DM_MAX_HANDSHAKING; i++) {
        if (s_pending_eph[i].used && s_pending_eph[i].peer_addr == peer_addr) {
            free_idx = i;
            break;
        }
        if (free_idx < 0 && !s_pending_eph[i].used)
            free_idx = i;
    }
    if (free_idx < 0) {
        /* Table sized to DM_MAX_HANDSHAKING, same cap dm_alloc enforces for
         * HANDSHAKING slots, so this should never actually trip: dm_alloc
         * would have already refused a new handshake before this is called. */
        ESP_LOGW(TAG, "Pending ephemeral table full, dropping entry for %08" PRIX32, peer_addr);
        return;
    }
    s_pending_eph[free_idx].peer_addr = peer_addr;
    memcpy(s_pending_eph[free_idx].eph_priv, eph_priv, 32);
    memcpy(s_pending_eph[free_idx].eph_pub, eph_pub, 32);
    s_pending_eph[free_idx].used = true;
}

static dm_pending_eph_t* pending_eph_lookup(uint32_t peer_addr) {
    for (int i = 0; i < DM_MAX_HANDSHAKING; i++) {
        if (s_pending_eph[i].used && s_pending_eph[i].peer_addr == peer_addr) {
            return &s_pending_eph[i];
        }
    }
    return NULL;
}

static void pending_eph_clear(uint32_t peer_addr) {
    for (int i = 0; i < DM_MAX_HANDSHAKING; i++) {
        if (s_pending_eph[i].used && s_pending_eph[i].peer_addr == peer_addr) {
            memset(&s_pending_eph[i], 0, sizeof(s_pending_eph[i]));
            return;
        }
    }
}

/*
 * Handshake dedup (SEC-C2 item 5). Returns 1 (dup, caller must drop without
 * reprocessing) or 0 (fresh, recorded so the next identical INIT dedups).
 * eph_pub_hash is a plain truncated SHA-256 over the ephemeral pubkey (a
 * public value): this is a dedup cache key, not an authentication tag, so
 * no HMAC/constant-time comparison is needed.
 */
static int hs_dedup_check_and_record(uint32_t src_addr, const uint8_t eph_pub[32],
                                     uint16_t ke_epoch, uint32_t now) {
    uint8_t digest[32] = {0};
    (void)crypto_sha256(eph_pub, 32, digest);
    uint32_t eph_hash;
    memcpy(&eph_hash, digest, 4);

    int lru = 0;
    for (int i = 0; i < DM_HS_DEDUP_MAX; i++) {
        dm_hs_dedup_entry_t* e = &s_hs_dedup[i];
        if (e->used && e->src_addr == src_addr && e->eph_pub_hash == eph_hash &&
            e->ke_epoch == ke_epoch) {
            return 1;
        }
        if (!e->used) {
            lru = i;
            break;
        }
        if (e->seen_ms < s_hs_dedup[lru].seen_ms)
            lru = i;
    }
    s_hs_dedup[lru].src_addr = src_addr;
    s_hs_dedup[lru].eph_pub_hash = eph_hash;
    s_hs_dedup[lru].ke_epoch = ke_epoch;
    s_hs_dedup[lru].seen_ms = now;
    s_hs_dedup[lru].used = true;
    return 0;
}

/*
 * B5 queue-and-trigger: queues a DM payload awaiting session establishment
 * and assigns it a real, trackable packet_id up front (unlike the legacy
 * awaiting-route queue_message, which has no onAck story at all). Queue
 * pressure evicts the oldest QUEUE_REASON_SESSION entry with the same
 * visible onAck failure a TTL expiry gets, rather than dropping the new
 * send silently; if every slot is a QUEUE_REASON_ROUTE entry, this send
 * fails visibly instead (a route-awaiting entry belongs to a different
 * subsystem's queue and is not evicted here).
 *
 * Known limitation (documented, not fixed here): the pkt_id returned here
 * is a tracking placeholder, not the pkt_id that eventually appears on the
 * wire once the session establishes and flush_session_queue actually calls
 * send_dm_packet (which mints its own pkt_id via next_packet_id, matching
 * every other send_*_packet in this file). A caller polling status by the
 * placeholder id will not see the real send's ack/delivery events; it will
 * see a failure notification via rerr_fastfail_notify if the queue entry
 * expires or is evicted, and nothing further if it is flushed successfully.
 */
static uint32_t queue_session_message(uint32_t dest_addr, const uint8_t* data, size_t len,
                                      int channel_idx) {
    int free_idx = -1;
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        int oldest = -1;
        for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
            if (s_queued_msgs[i].used && s_queued_msgs[i].reason == QUEUE_REASON_SESSION &&
                (oldest < 0 || s_queued_msgs[i].timestamp < s_queued_msgs[oldest].timestamp)) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            ESP_LOGW(TAG,
                     "Message queue full (no evictable session entry), failing DM for %08" PRIX32,
                     dest_addr);
            return 0;
        }
        ESP_LOGW(TAG, "Message queue full, evicting oldest awaiting-session entry for %08" PRIX32,
                 s_queued_msgs[oldest].dest_addr);
        rerr_fastfail_notify(s_queued_msgs[oldest].pkt_id, "queue_full", NULL);
        free_idx = oldest;
    }

    uint32_t pkt_id = next_packet_id();
    s_queued_msgs[free_idx].dest_addr = dest_addr;
    memcpy(s_queued_msgs[free_idx].data, data, len);
    s_queued_msgs[free_idx].len = len;
    s_queued_msgs[free_idx].timestamp = now_ms();
    s_queued_msgs[free_idx].reason = QUEUE_REASON_SESSION;
    s_queued_msgs[free_idx].pkt_id = pkt_id;
    s_queued_msgs[free_idx].channel_idx = (int16_t)channel_idx;
    s_queued_msgs[free_idx].used = true;
    ESP_LOGI(TAG, "Queued DM for %08" PRIX32 " (awaiting session)", dest_addr);
    return pkt_id;
}

/* Sends every QUEUE_REASON_SESSION entry for dest_addr now that a session
 * is ACTIVE. Takes s_dm_mutex fresh per entry (not held across the whole
 * loop) so a long flush never blocks handle_ke_envelope/handshake_worker_task
 * for longer than a single send. */
static void flush_session_queue(uint32_t dest_addr) {
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used || s_queued_msgs[i].reason != QUEUE_REASON_SESSION ||
            s_queued_msgs[i].dest_addr != dest_addr) {
            continue;
        }

        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(&s_dm_table, dest_addr);
        uint32_t pkt_id = 0;
        if (sess && sess->state == DM_STATE_ACTIVE) {
            pkt_id = send_dm_packet(dest_addr, s_queued_msgs[i].data, s_queued_msgs[i].len, sess);
        }
        xSemaphoreGive(s_dm_mutex);

        if (pkt_id != 0) {
            ESP_LOGI(TAG, "Flushed queued DM to %08" PRIX32 " (%u bytes)", dest_addr,
                     (unsigned)s_queued_msgs[i].len);
            msg_store_add_ex2(dest_addr, MSG_DIR_OUTGOING, (const char*)s_queued_msgs[i].data,
                              s_queued_msgs[i].len, 0, 0, pkt_id, MSG_STATUS_SENT,
                              s_queued_msgs[i].channel_idx);
        } else {
            ESP_LOGW(TAG, "Failed to flush queued DM to %08" PRIX32, dest_addr);
            rerr_fastfail_notify(s_queued_msgs[i].pkt_id, "session_send_failed", NULL);
        }
        s_queued_msgs[i].used = false;
    }
}

/*
 * Builds and sends a first-contact INIT (SEC-C2 handshake transport, under
 * the channel key via send_ke_envelope). Always first-contact: the only
 * caller (mesh_send_dm) reaches this exclusively for a peer with no
 * existing s_dm_table slot at all, so there is never a cached peer_id_pub
 * to rekey against here. Proactive rekey of an already-ACTIVE session is a
 * different trigger, out of this task's wiring scope.
 */
static void initiate_dm_handshake(uint32_t dest_addr, int channel_idx) {
    bramble_identity_t my_eph;
    crypto_generate_identity(&my_eph);

    bramble_key_exchange_t init;
    if (dm_build_init(s_identity, my_eph.public_key, my_eph.private_key, dest_addr, 0, NULL,
                      &init) != 0) {
        ESP_LOGE(TAG, "dm_build_init failed for %08" PRIX32, dest_addr);
        return;
    }

    pending_eph_store(dest_addr, my_eph.private_key, my_eph.public_key);

    if (send_ke_envelope(dest_addr, channel_idx, &init) == 0) {
        ESP_LOGW(TAG, "Failed to send INIT to %08" PRIX32, dest_addr);
    }
}

/*
 * SEC-C2 queue-and-trigger (item 4): the ONLY place a unicast DM decides
 * between the session path and queue-and-handshake. NEVER falls back to
 * the channel key for a DM payload: an ACTIVE session sends via
 * send_dm_packet; anything else queues and (if not already handshaking)
 * triggers an INIT, or fails visibly if the handshaking cap is reached.
 */
static uint32_t mesh_send_dm(int channel_idx, uint32_t dest_addr, const uint8_t* data, size_t len) {
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Fragmentation under a session key is out of this task's scope
         * (DM chat payloads are short); fail visibly rather than silently
         * truncating or falling back to the channel key. */
        ESP_LOGW(TAG, "DM payload too large for the session path: %u bytes", (unsigned)len);
        return 0;
    }

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* sess = dm_lookup(&s_dm_table, dest_addr);
    if (sess && sess->state == DM_STATE_ACTIVE) {
        uint32_t pkt_id = send_dm_packet(dest_addr, data, len, sess);
        xSemaphoreGive(s_dm_mutex);
        if (pkt_id != 0) {
            msg_store_add_ex2(dest_addr, MSG_DIR_OUTGOING, (const char*)data, len, 0, 0, pkt_id,
                              MSG_STATUS_SENT, (int16_t)channel_idx);
        }
        return pkt_id;
    }

    bool handshake_in_progress = sess && sess->state == DM_STATE_HANDSHAKING;
    dm_session_t* hs = sess;
    if (!hs) {
        hs = dm_alloc(&s_dm_table, dest_addr, now_ms());
        if (hs)
            hs->state = DM_STATE_HANDSHAKING;
    }
    xSemaphoreGive(s_dm_mutex);

    if (!hs) {
        /* M4 DoS defense: handshaking cap reached. Fail the send visibly;
         * never transmit this payload under the channel key instead. */
        ESP_LOGW(TAG, "No session and handshaking cap reached for %08" PRIX32, dest_addr);
        return 0;
    }

    uint32_t pkt_id = queue_session_message(dest_addr, data, len, channel_idx);
    if (pkt_id == 0) {
        return 0;
    }

    if (!handshake_in_progress) {
        initiate_dm_handshake(dest_addr, channel_idx);
    }

    /* Still store in msg_store so UI shows it as pending, same convention
     * as the awaiting-route path. */
    msg_store_add(dest_addr, MSG_DIR_OUTGOING, (const char*)data, len, 0, 0);
    return pkt_id;
}

/*
 * Responder side of the INIT/RESP state machine (runs on
 * handshake_worker_task, never inline on the mesh RX loop: M7). Item 2
 * downgrade defense: have_peer_id is derived from whatever s_dm_table
 * already holds for src_addr at the moment of the check, so a zero-tag
 * INIT can never be accepted as first-contact against an already-known
 * identity.
 */
static void process_ke_init(uint32_t src_addr, int channel_idx, const bramble_key_exchange_t* init,
                            const uint8_t* pinned_x25519_or_null) {
    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* existing = dm_lookup(&s_dm_table, src_addr);
    int have_peer_id = dm_session_has_peer_id(existing);
    /* Zero-init: peer_id_pub is only read (passed below) when have_peer_id is
     * set, and it is filled here in exactly that case, so no uninitialized
     * read occurs. The initializer makes that invariant explicit and silences
     * a cppcheck uninitvar warning it cannot otherwise prove. */
    uint8_t peer_id_pub[32] = {0};
    if (have_peer_id)
        memcpy(peer_id_pub, existing->peer_id_pub, 32);
    xSemaphoreGive(s_dm_mutex);

    int vrc = dm_verify_init(init, s_identity, have_peer_id, have_peer_id ? peer_id_pub : NULL,
                             pinned_x25519_or_null);
    if (vrc == DM_VERIFY_ERR_PIN_MISMATCH) {
        /* Phase 4 DM key continuity RED FLAG: this address has an
         * attestation-verified pinned X25519 key and the handshake showed
         * up with a DIFFERENT one. Refuse the session loudly; a silent
         * accept here would let a keyed insider splice itself into a
         * known peer's DMs. */
        ESP_LOGW(TAG,
                 "DM KEY CONTINUITY: INIT from %08" PRIX32 " does not match its pinned identity"
                 " key, session REFUSED",
                 src_addr);
        return;
    }
    if (vrc != 0) {
        /* One-sided desync recovery (DM self-heal PART 2). We hold a cached
         * peer_id for src_addr (have_peer_id) so we took dm_verify_init's
         * strict tag path, yet the INIT is from an ATTESTATION-PINNED peer:
         * pinned_x25519_or_null is set and, since we are past the
         * PIN_MISMATCH gate above, the INIT's long_term_pubkey MATCHED it.
         * So this is cryptographically that peer re-initiating fresh (it
         * rebooted / lost its half of the session, e.g. after the receiver
         * self-heal triggers a first-contact INIT). Tear our stale session
         * down and re-accept as first contact so DMs heal instead of failing
         * forever. Rate-limited: long_term_pubkey is public, so a spoofed
         * INIT naming a pinned peer must not be spammable into a
         * session-teardown DoS; the worst case is one bounded teardown that
         * the next genuine handshake repairs (the resulting session is only
         * usable by whoever can complete the DH, i.e. the real peer). */
        if (have_peer_id && pinned_x25519_or_null && dm_rehandshake_rate_ok(src_addr, now_ms())) {
            xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
            dm_session_teardown(&s_dm_table, src_addr);
            xSemaphoreGive(s_dm_mutex);
            ESP_LOGI(TAG,
                     "DM session desync: pinned peer %08" PRIX32 " re-initiated; tore down stale"
                     " session, re-accepting as first contact (self-heal)",
                     src_addr);
            vrc = dm_verify_init(init, s_identity, 0, NULL, pinned_x25519_or_null);
        }
        if (vrc != 0) {
            ESP_LOGW(TAG, "INIT verify failed from %08" PRIX32, src_addr);
            return;
        }
    }

    bramble_identity_t my_eph;
    crypto_generate_identity(&my_eph);
    uint16_t ke_epoch = (uint16_t)init->key_id;

    bramble_key_exchange_t resp;
    uint8_t session_key[32];
    if (dm_build_resp(s_identity, my_eph.public_key, my_eph.private_key, init, ke_epoch, &resp,
                      session_key) != 0) {
        ESP_LOGE(TAG, "dm_build_resp failed for %08" PRIX32, src_addr);
        return;
    }

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* sess = dm_alloc(&s_dm_table, src_addr, now_ms());
    if (sess) {
        memcpy(sess->session_key, session_key, 32);
        memcpy(sess->peer_id_pub, init->long_term_pubkey, 32);
        sess->established_ms = now_ms();
        sess->msg_count = 0;
        sess->ke_epoch = ke_epoch;
        sess->state = DM_STATE_ACTIVE;
        sess->verified = 0; /* SAS confirmation is a separate UX step, not wired here */
    }
    xSemaphoreGive(s_dm_mutex);

    if (!sess) {
        ESP_LOGW(TAG, "Handshaking cap reached, cannot establish session with %08" PRIX32,
                 src_addr);
        return;
    }

    if (send_ke_envelope(src_addr, channel_idx, &resp) == 0) {
        ESP_LOGW(TAG, "Failed to send RESP to %08" PRIX32, src_addr);
    }

    flush_session_queue(src_addr);
}

/*
 * Initiator side: verifies a RESP against the ephemeral we generated when
 * we sent the matching INIT (dm_pending_eph_t; dm_session_t itself has no
 * field for in-flight handshake material, see its declaration above).
 */
static void process_ke_resp(uint32_t src_addr, const bramble_key_exchange_t* resp,
                            const uint8_t* pinned_x25519_or_null) {
    dm_pending_eph_t* pe = pending_eph_lookup(src_addr);
    if (!pe) {
        ESP_LOGW(TAG, "RESP from %08" PRIX32 " with no matching pending INIT", src_addr);
        return;
    }

    uint16_t ke_epoch = (uint16_t)resp->key_id;
    uint8_t session_key[32];
    int vrc = dm_verify_resp(resp, s_identity, pe->eph_priv, pe->eph_pub, ke_epoch,
                             pinned_x25519_or_null, session_key);
    if (vrc == DM_VERIFY_ERR_PIN_MISMATCH) {
        /* Same red flag as process_ke_init: pinned peer, different DM key. */
        ESP_LOGW(TAG,
                 "DM KEY CONTINUITY: RESP from %08" PRIX32 " does not match its pinned identity"
                 " key, session REFUSED",
                 src_addr);
        return;
    }
    if (vrc != 0) {
        ESP_LOGW(TAG, "RESP verify failed from %08" PRIX32, src_addr);
        return;
    }

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* sess = dm_lookup(&s_dm_table, src_addr);
    if (sess) {
        memcpy(sess->session_key, session_key, 32);
        memcpy(sess->peer_id_pub, resp->long_term_pubkey, 32);
        sess->established_ms = now_ms();
        sess->msg_count = 0;
        sess->ke_epoch = ke_epoch;
        sess->state = DM_STATE_ACTIVE;
        sess->verified = 0;
    }
    xSemaphoreGive(s_dm_mutex);

    pending_eph_clear(src_addr);

    if (!sess) {
        ESP_LOGW(TAG, "Session slot for %08" PRIX32 " vanished before RESP could complete it",
                 src_addr);
        return;
    }

    flush_session_queue(src_addr);
}

/* M7 offload: drains handshake work items posted by handle_ke_envelope.
 * Low priority, small stack: the only work here is the occasional
 * four-X25519-mult handshake, never on the mesh RX critical path. */
static void handshake_worker_task(void* arg) {
    (void)arg;
    dm_handshake_work_item_t item;
    for (;;) {
        if (xQueueReceive(s_handshake_work_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.initiate) {
            /* DM self-heal: our session with this peer desynced (we could not
             * decrypt its DM), so re-initiate a fresh handshake off the RX task. */
            initiate_dm_handshake(item.src_addr, item.channel_idx);
            continue;
        }
        const uint8_t* pinned = item.have_pin ? item.pinned_x25519 : NULL;
        if (item.msg.ke_type == KE_TYPE_INIT) {
            process_ke_init(item.src_addr, item.channel_idx, &item.msg, pinned);
        } else if (item.msg.ke_type == KE_TYPE_RESP) {
            process_ke_resp(item.src_addr, &item.msg, pinned);
        }
    }
}

/*
 * RX entry point for APP_TYPE_KE inner payloads (SEC-C2 handshake-in-DATA).
 * Does only cheap parsing/validation here; the DH-heavy INIT/RESP state
 * machine runs on handshake_worker_task (M7). src_addr is the OUTER DATA
 * envelope's already-authenticated src_addr (from channel_msg_decrypt's
 * AAD binding), not yet trusted to equal the inner struct's own claimed
 * src_addr until checked below.
 */
static void handle_ke_envelope(uint32_t src_addr, int channel_idx, const uint8_t* data,
                               size_t data_len) {
    bramble_key_exchange_t msg;
    if (bramble_key_exchange_deserialize(&msg, data, data_len) != ESP_OK) {
        ESP_LOGW(TAG, "Malformed KE envelope from %08" PRIX32, src_addr);
        return;
    }
    if (msg.ke_type != KE_TYPE_INIT && msg.ke_type != KE_TYPE_RESP) {
        ESP_LOGW(TAG, "Unknown ke_type %u from %08" PRIX32, msg.ke_type, src_addr);
        return;
    }
    /* Outer/inner src_addr consistency: closes a confusion vector where a
     * valid channel-key sender embeds a KE payload claiming a different
     * address than the one the outer envelope's AAD already authenticated. */
    if (msg.src_addr != src_addr) {
        ESP_LOGW(TAG, "KE envelope src_addr mismatch: outer=%08" PRIX32 " inner=%08" PRIX32,
                 src_addr, msg.src_addr);
        return;
    }
    /* Item 3: reject self-addressed (role-confusion-at-dispatch defense). */
    if (src_addr == s_identity->address) {
        ESP_LOGW(TAG, "Dropping self-addressed KE envelope");
        return;
    }

    if (msg.ke_type == KE_TYPE_INIT) {
        uint16_t ke_epoch = (uint16_t)msg.key_id;
        if (hs_dedup_check_and_record(src_addr, msg.ephemeral_pubkey, ke_epoch, now_ms())) {
            ESP_LOGD(TAG, "Duplicate INIT from %08" PRIX32 ", not re-running handshake", src_addr);
            return;
        }
    }

    dm_handshake_work_item_t item;
    memset(&item, 0, sizeof(item));
    item.src_addr = src_addr;
    item.channel_idx = channel_idx;
    item.msg = msg;
    /* Phase 4 DM key continuity: snapshot the pinned X25519 key for this
     * peer HERE, on the mesh task (the only mutator of s_identity_pins),
     * so the handshake worker verifies against an immutable copy instead
     * of reading the pin store cross-thread. No pin = NULL downstream =
     * TOFU-grade first contact (stated residual until the peer's
     * attestation is heard and pinned). */
    const identity_pin_t* pin = identity_store_lookup(&s_identity_pins, src_addr);
    if (pin) {
        item.have_pin = true;
        memcpy(item.pinned_x25519, pin->x25519_pub, sizeof(item.pinned_x25519));
    }
    if (xQueueSend(s_handshake_work_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Handshake work queue full, dropping KE from %08" PRIX32, src_addr);
    }
}

bool mesh_supports_delivery_event_sync(void) { return true; }

uint32_t mesh_delivery_events_latest_seq(void) {
    uint32_t latest = 0u;
    if (!s_delivery_event_mutex)
        return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    latest = delivery_event_ring_latest_seq(s_delivery_event_ring);
    xSemaphoreGive(s_delivery_event_mutex);
    return latest;
}

size_t mesh_delivery_events_list_since(uint32_t since_event_seq, delivery_event_record_t* out,
                                       size_t out_max) {
    size_t count = 0u;
    if (!s_delivery_event_mutex)
        return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    count = delivery_event_ring_list_since(s_delivery_event_ring, since_event_seq, out, out_max);
    xSemaphoreGive(s_delivery_event_mutex);
    return count;
}

uint32_t mesh_get_last_broadcast_id(void) {
    (void)s_last_broadcast_frag_msg_id;
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

void mesh_emit_broadcast_delivery_notification(uint32_t src_addr, uint32_t broadcast_id,
                                               int8_t rssi_at_dest, uint8_t hop_count,
                                               const uint32_t* relay_path) {
    if (s_broadcast_telemetry_mode == BROADCAST_TELEMETRY_OFF) {
        return;
    }

    char src_buf[12], id_buf[12], hop_buf[12];
    cJSON* params = cJSON_CreateObject();
    snprintf(src_buf, sizeof(src_buf), "%08" PRIX32, src_addr);
    snprintf(id_buf, sizeof(id_buf), "%08" PRIX32, broadcast_id);
    cJSON_AddStringToObject(params, "recipient", src_buf);
    cJSON_AddStringToObject(params, "broadcast_id", id_buf);
    cJSON_AddStringToObject(params, "status", "delivered");
    cJSON_AddNumberToObject(params, "rssi_at_dest", rssi_at_dest);

    if (s_broadcast_telemetry_mode == BROADCAST_TELEMETRY_PATH_SAMPLED && hop_count > 0 &&
        relay_path) {
        uint8_t bounded_hops =
            (hop_count > DELIVERY_RECEIPT_MAX_HOPS) ? DELIVERY_RECEIPT_MAX_HOPS : hop_count;
        cJSON* path = cJSON_AddArrayToObject(params, "relayPath");
        for (uint8_t i = 0; i < bounded_hops; i++) {
            cJSON* hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, relay_path[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            cJSON_AddItemToArray(path, hop);
        }
    }

    record_broadcast_delivery_event(src_addr, broadcast_id, hop_count, relay_path);

    rpc_notify("bramble.onBroadcastDelivery", params);
    cJSON_Delete(params);
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
        /* Long message — split into fragments */
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

        s_last_broadcast_frag_msg_id = 0;

        /* Send each fragment with pacing */
        for (int i = 0; i < num_frags; i++) {
            uint32_t pkt_id =
                send_data_packet(0xFFFFFFFF, frags[i].data, frags[i].len, &s_channels[0], 0x01);
            if (i == 0 && pkt_id != 0) {
                s_last_broadcast_id = pkt_id;
                s_last_broadcast_frag_msg_id = msg_id;
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

        /* Store the full message in message store */
        msg_store_add_ex2(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, (const char*)data, len, 0, 0, 0,
                          MSG_STATUS_NONE, 0);
        return 0;
    }

    /* Short message — fast path (no fragmentation) */
    uint32_t pkt_id = send_data_packet(0xFFFFFFFF, data, len, &s_channels[0], 0x01);
    if (pkt_id != 0) {
        s_last_broadcast_id = pkt_id;
        s_last_broadcast_frag_msg_id = 0;
        recent_broadcast_record(pkt_id);
        msg_store_add_ex2(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, (const char*)data, len, 0, 0, 0,
                          MSG_STATUS_NONE, 0);
    }
    return pkt_id ? 0 : -1;
}

uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t* data, size_t len) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGE(TAG, "Invalid channel index: %d (count=%d)", channel_idx, s_num_channels);
        return 0;
    }

    /* SEC-C2: every unicast send (whether via mesh_send_message's default
     * channel or an explicit channel picked by rpc_methods.c) converges
     * here before any bytes are encrypted. Broadcasts (dest_addr ==
     * 0xFFFFFFFF) have no peer to hold a session with and always fall
     * through to the channel-key path below unchanged. */
    if (dest_addr != 0xFFFFFFFFu) {
        return mesh_send_dm(channel_idx, dest_addr, data, len);
    }

    /* Check if fragmentation is needed */
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Long message — split into fragments */
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

        if (dest_addr == 0xFFFFFFFFu) {
            s_last_broadcast_frag_msg_id = 0;
        }

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
                        s_last_broadcast_frag_msg_id = msg_id;
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
            msg_store_add_ex2(dest_addr,
                              (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_DIR_BROADCAST_OUT
                                                                            : MSG_DIR_OUTGOING,
                              (const char*)data, len, 0, 0, first_pkt_id,
                              (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_STATUS_NONE
                                                                            : MSG_STATUS_SENT,
                              (int16_t)channel_idx);
        }
        return first_pkt_id;
    }

    /* Short message — fast path (no fragmentation) */
    uint32_t pkt_id = send_data_packet(dest_addr, data, len, &s_channels[channel_idx], 0x01);
    if (pkt_id != 0) {
        if (dest_addr == 0xFFFFFFFFu) {
            s_last_broadcast_id = pkt_id;
            s_last_broadcast_frag_msg_id = 0;
            recent_broadcast_record(pkt_id);
        }
        msg_store_add_ex2(dest_addr,
                          (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_DIR_BROADCAST_OUT
                                                                        : MSG_DIR_OUTGOING,
                          (const char*)data, len, 0, 0, pkt_id,
                          (dest_addr == 0xFFFFFFFF && channel_idx == 0) ? MSG_STATUS_NONE
                                                                        : MSG_STATUS_SENT,
                          (int16_t)channel_idx);
    }
    return pkt_id;
}

static int queue_message(uint32_t dest_addr, const uint8_t* data, size_t len) {
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used) {
            s_queued_msgs[i].dest_addr = dest_addr;
            memcpy(s_queued_msgs[i].data, data, len);
            s_queued_msgs[i].len = len;
            s_queued_msgs[i].timestamp = now_ms();
            s_queued_msgs[i].reason = QUEUE_REASON_ROUTE;
            s_queued_msgs[i].pkt_id = 0;
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

uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len) {
    if (s_num_channels == 0) {
        ESP_LOGE(TAG, "No channels initialized");
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
        /* Not a direct neighbor — need routing */
        route_entry_t* route = route_lookup(&s_routes, dest_addr);
        if (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING) {
            /* No route — start discovery and queue the message */
            if (!discovery_lookup(&s_pending_disc, dest_addr)) {
                initiate_discovery(dest_addr);
            }
            queue_message(dest_addr, data, len);
            /* Still store in msg_store so UI shows it as pending */
            msg_store_add(dest_addr, MSG_DIR_OUTGOING, (const char*)data, len, 0, 0);
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

/* Nonce counter NVS persistence: reserve-ahead ceiling under NVS_NS_NONCE.
 * Not-found (first boot) resumes from ceiling 0, matching nonce_counter's own
 * zero-initialized static state. */
static int mesh_nonce_read(uint64_t* ceiling_out, void* ctx) {
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NONCE, NVS_READONLY, &h) != ESP_OK) {
        *ceiling_out = 0;
        return 0;
    }
    size_t len = sizeof(*ceiling_out);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_NONCE_CEILING, ceiling_out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        *ceiling_out = 0;
    }
    return 0;
}

static int mesh_nonce_write(uint64_t ceiling, void* ctx) {
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NONCE, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    nvs_set_blob(h, NVS_KEY_NONCE_CEILING, &ceiling, sizeof(ceiling));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

/*
 * Mandatory-provisioning (Task 2): consolidate boot-time key load onto the
 * network_key component (single source of truth for the NVS namespace/key and
 * the in-memory provisioning state). A stored key -> provisioned; none stored
 * -> the node stays UNPROVISIONED and INERT (no public-PSK fallback), which is
 * the shipped default until an operator provisions one. Logged clearly so the
 * boot state is unambiguous (a status field for Task 3's provisioning UX).
 */
static void mesh_load_network_key(void) {
    if (network_key_load_from_nvs() == 0) {
        ESP_LOGI(TAG, "Network key loaded from NVS (provisioned)");
    } else {
        ESP_LOGW(TAG, "No network key provisioned: node is INERT until provisioned "
                      "(setNetworkKey or generate)");
    }
}

void mesh_rederive_beacon_key(void) {
    /* SEC-H2: derive the beacon HMAC subkey from the current network key with
     * domain-separation label "bramble-beacon-v2". Called at init AND after a
     * runtime setNetworkKey so provisioning takes effect for beacons without a
     * reboot.
     *
     * Mandatory-provisioning (Task 2): the public-PSK fallback is GONE. When
     * unprovisioned there is no beacon key -- zero it so a stale key can never
     * be reused, and the node neither beacons (send_beacon is gated) nor
     * accepts beacons (handle_beacon is gated). Do NOT derive from
     * BRAMBLE_PUBLIC_CHANNEL_PSK: that would re-introduce a control-plane
     * fallback key. */
    if (network_key_is_provisioned()) {
        uint8_t net_key[32];
        network_key_get(net_key);
        const char* salt = "bramble-beacon-v2";
        crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), net_key, sizeof(net_key), NULL, 0,
                           s_beacon_key, sizeof(s_beacon_key));
        ESP_LOGI(TAG, "Beacon HMAC key derived from the provisioned network key");
    } else {
        memset(s_beacon_key, 0, sizeof(s_beacon_key));
        ESP_LOGW(TAG, "unprovisioned: no beacon key (node inert until provisioned)");
    }
}

void mesh_trigger_attestation(void) {
    /* Re-announce with the current identity + cert. Budget-gated like every
     * attestation (attempt_identity_attestation applies the same TX gate as
     * the periodic path), so a burst of setEndorsement calls cannot flood the
     * air. Inert until provisioned (send_identity_attestation gates on the
     * network key). */
    attempt_identity_attestation(now_ms());
}

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
    dm_table_init(&s_dm_table);
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

    /* Pin to CPU1 — leave CPU0 for UI/display */
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

void mesh_get_routes(routing_table_t* out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_routes;
    xSemaphoreGive(s_state_mutex);
}

void mesh_get_location_state(location_manager_t* out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_location_mgr;
    xSemaphoreGive(s_state_mutex);
}

int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_num_channels >= MAX_CHANNELS) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    bramble_channel_t* ch = &s_channels[s_num_channels];
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
        if (crypto_random(ch->key, BRAMBLE_KEY_SIZE) != 0) {
            /* Entropy not ready (pre-RF window): do NOT install a zeroed key.
             * Skip creating this channel; it is retried once an RF entropy
             * source is up and the gate re-opens (SEC-L1). Release the mutex
             * taken above before returning: this function's other early-return
             * (MAX_CHANNELS, above) does the same. */
            ESP_LOGW(TAG, "channel key gen deferred: entropy not ready");
            xSemaphoreGive(s_state_mutex);
            return -1;
        }
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
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) !=
        0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS");
    }
    mesh_persist_channel_psk_flags();

    /* Catch-up budget buckets are indexed by channel position */
    channel_msg_catchup_reset();

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
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) !=
        0) {
        ESP_LOGW(TAG, "Failed to persist channels to NVS after removal");
    }
    mesh_persist_channel_psk_flags();

    /* Indices shifted: stale per-index budgets would misattribute */
    channel_msg_catchup_reset();

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Channel removed: idx=%d, %d remaining", index, s_num_channels);
    return 0;
}

int mesh_get_channel_count(void) { return s_num_channels; }

const char* mesh_get_channel_name(int index) {
    if (index < 0 || index >= s_num_channels)
        return NULL;
    if (s_channel_names[index][0])
        return s_channel_names[index];

    static char name_buf[20];
    if (index == 0)
        return "Broadcast";

    nvs_handle_t ch_nvs;
    if (nvs_open(NVS_NS_CHANNEL, NVS_READONLY, &ch_nvs) != ESP_OK) {
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

int mesh_get_channel_security(int index, bool* has_psk, uint16_t* epoch) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_num_channels) {
        xSemaphoreGive(s_state_mutex);
        return -1;
    }

    if (has_psk)
        *has_psk = s_channel_has_psk[index];
    if (epoch)
        *epoch = s_channels[index].epoch;

    xSemaphoreGive(s_state_mutex);
    return 0;
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

void mesh_set_mailbox(bool enabled) {
    s_mailbox_enabled = enabled;
    ESP_LOGI(TAG, "Mailbox runtime: %s", enabled ? "enabled" : "disabled");
}

bool mesh_get_mailbox(void) { return s_mailbox_enabled; }

void mesh_set_flood_transport(bool enabled) {
    s_flood_transport = enabled;
    ESP_LOGI(TAG, "Flood transport runtime: %s", enabled ? "enabled" : "disabled");
}

bool mesh_get_flood_transport(void) { return s_flood_transport; }

void mesh_set_flood_hop_limit(uint32_t hops) { s_flood_hop_limit = flood_hop_limit_clamp(hops); }

uint8_t mesh_get_flood_hop_limit(void) { return s_flood_hop_limit; }

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

    /* Deny behavior: a probe sweep is on-demand diagnostics; a denied
     * round is simply skipped and reported via the per-round rc log. */
    int rc = mesh_tx(buf, HEADER_SIZE + 5, TX_KIND_PROBE);
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

static void handle_probe(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 4)
        return;

    bramble_header_t header;
    bramble_header_deserialize(&header, data, len);

    uint32_t src_addr;
    memcpy(&src_addr, data + HEADER_SIZE, 4);
    uint8_t probe_round = (len >= HEADER_SIZE + 5) ? data[HEADER_SIZE + 4] : 1;

    char src_buf[12], me_buf[12];
    ESP_LOGI(TAG, "PROBE RX pid=%08" PRIX32 " round=%u src=%s me=%s hop=%u rssi=%d snr=%d",
             header.packet_id, (unsigned)probe_round, addr_hex(src_addr, src_buf, sizeof(src_buf)),
             addr_hex(s_identity->address, me_buf, sizeof(me_buf)), (unsigned)header.hop_limit,
             (int)rssi, (int)snr);

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

    /* Defer the reply burst (slot delay + jitter + 3 sends 140ms apart) onto
     * the probe-reply timer/queue so the mesh task is never blocked (DES-15).
     * Same slotting/jitter/spacing as before; only the blocking is removed. */
    queue_probe_reply(buf, HEADER_SIZE + 6, s_identity->address);

    ESP_LOGI(TAG, "PROBE ACK QUEUED pid=%08" PRIX32 " round=%u to=%s from=%s hops=1",
             header.packet_id, (unsigned)probe_round, addr_hex(src_addr, src_buf, sizeof(src_buf)),
             addr_hex(s_identity->address, me_buf, sizeof(me_buf)));

    /* Forward probe if hop limit allows */
    if (header.hop_limit > 1) {
        bramble_header_t fwd = header;
        fwd.hop_limit--;
        uint8_t fwd_buf[20];
        bramble_header_serialize(&fwd, fwd_buf, HEADER_SIZE);
        memcpy(fwd_buf + HEADER_SIZE, data + HEADER_SIZE, 4);
        mesh_tx(fwd_buf, HEADER_SIZE + 4, TX_KIND_PROBE);
        ESP_LOGI(TAG, "PROBE FWD pid=%08" PRIX32 " new_hop=%u", header.packet_id,
                 (unsigned)fwd.hop_limit);
    }
}

static void handle_probe_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 5)
        return;

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
            mesh_tx(fwd_buf, len, TX_KIND_PROBE_REPLY);
            ESP_LOGI(TAG, "PROBE ACK FWD pid=%08" PRIX32 " dest=%s hop=%u", header.packet_id,
                     addr_hex(header.dest_addr, dst_buf, sizeof(dst_buf)), (unsigned)fwd.hop_limit);
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
    if (probe_round < 1 || probe_round > PROBE_SWEEP_ROUNDS)
        probe_round = 1;

    /* Never include self in probe responders. */
    if (resp_addr == s_identity->address) {
        ESP_LOGI(TAG, "PROBE ACK RX ignored self responder pid=%08" PRIX32, header.packet_id);
        return;
    }

    uint32_t latency = now_ms() - s_probe_sent_ms;

    /* Upsert by responder addr: one logical row per responder. */
    probe_results_upsert(s_probe_results, &s_probe_result_count, MAX_PROBE_RESULTS, resp_addr, hops,
                         rssi, snr, latency, probe_round);

    char buf[12];
    ESP_LOGI(TAG,
             "PROBE ACK RX from=%s round=%u hops=%u rssi=%d snr=%d latency=%" PRIu32
             "ms pid=%08" PRIX32,
             addr_hex(resp_addr, buf, sizeof(buf)), (unsigned)probe_round, (unsigned)hops,
             (int)rssi, (int)snr, now_ms() - s_probe_sent_ms, header.packet_id);

    /* Emit notification */
    cJSON* params = cJSON_CreateObject();
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
    if (channel_storage_save(s_channels, s_num_channels, s_channel_names, s_default_channel_idx) !=
        0) {
        ESP_LOGW(TAG, "Failed to persist default channel to NVS");
    }

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI("mesh", "Default channel set to idx=%d (broadcast remains public channel 0)", index);
    return 0;
}

const char* mesh_get_node_name(void) {
    if (s_node_name[0] == '\0')
        return NULL;
    return s_node_name;
}

bool mesh_get_network_time_ms(int64_t* out_ms) {
    if (!out_ms || !s_timesync.synchronized)
        return false;
    *out_ms = timesync_get_network_time(&s_timesync, now_ms());
    return true;
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

int mesh_get_channel_info(int* default_idx) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int count = s_num_channels;
    if (default_idx) {
        *default_idx = s_default_channel_idx;
    }
    xSemaphoreGive(s_state_mutex);
    return count;
}

/* ── Traffic debug access ───────────────────────────────────────────── */

static void traffic_event_notify(const traffic_event_t* evt, void* ctx) {
    (void)ctx;

    /* Only send notifications if debug is enabled */
    if (!traffic_debug_is_enabled(&s_traffic_debug)) {
        return;
    }

    /* Build notification payload */
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "seq", evt->seq);
    cJSON_AddNumberToObject(params, "timestamp_ms", evt->timestamp_ms);
    cJSON_AddNumberToObject(params, "pkt_type", evt->pkt_type);

    /* Category as string */
    static const char* cat_names[] = {"beacon", "timesync",    "routing", "ack",
                                      "chat",   "maintenance", "other"};
    if (evt->category < 7) {
        cJSON_AddStringToObject(params, "category", cat_names[evt->category]);
    } else {
        cJSON_AddStringToObject(params, "category", "unknown");
    }

    /* Airtime tier as string */
    static const char* tier_names[] = {"none", "normal", "critical", "broadcast"};
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

traffic_debug_t* mesh_get_traffic_debug(void) { return &s_traffic_debug; }

void mesh_traffic_debug_set_config(bool enabled, bool include_tx, bool include_rx,
                                   uint8_t sample_rate) {
    /* F26: Update in-memory state under mutex, then do NVS I/O outside */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    traffic_debug_enable(&s_traffic_debug, enabled);
    xSemaphoreGive(s_state_mutex);

    /* Persist config to NVS (flash I/O — do NOT hold mesh mutex) */
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

    /* Load other config from NVS (flash I/O — do NOT hold mesh mutex) */
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
