/**
 * mesh_internal.h: internal shared surface across the mesh_task translation
 * units.
 *
 * mesh_task.c owns the shared protocol state and the orchestration core (the
 * RX dispatch, the main task loop, init, and the send/TX API). The
 * per-responsibility modules around it:
 *
 *   mesh_beacon.c       beacons, identity attestation, adaptive beacon policy
 *   mesh_reliability.c  ACKs, delivery receipts, delivery-event ring, telemetry
 *   mesh_probe.c        neighbor probe sweep + probe-reply queue
 *   mesh_dm.c           DM handshake / session / ratchet plumbing
 *   mesh_routing.c      RREQ/RREP/RERR, jittered RREQ + flood relay, forwarding
 *   mesh_location.c     location share TX/RX, persistence, policy tick
 *   mesh_mailbox.c      store-and-forward mailbox glue
 *   mesh_persist.c      NVS: nonce ceiling, pin store, replay store, net key
 *   mesh_channels.c     channel table management
 *   mesh_rollcall.c     attested roll-call: announce rounds, staggered
 *                       identity-signed answers, and the initiator ledger
 *
 * These modules include this header to reach the shared state and each other's
 * cross-module entry points. This is deliberately the ONE place visibility is
 * widened: anything not declared here stays static to its own translation
 * unit.
 */
#ifndef MESH_INTERNAL_H
#define MESH_INTERNAL_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#include "mesh_task.h"
#include "util.h"
#include "rreq_pseudonym.h"
#include "beacon_policy_calc.h"
#include "probe_results.h"
#include "probe_reply.h"
#include "parked_retry.h"
#include "broadcast_delivery_receipt.h"
#include "radio.h"
#include "tx_gate.h"
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
#include "mesh_rollcall.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

/* ── Shared configuration ───────────────────────────────────────────── */

#define MESH_TASK_PRIORITY 5
#define RECEIPT_QUEUE_CAPACITY 8
/* How many times one delivery-receipt attempt may be re-scheduled because
 * tx_gate found the channel busy (TX_GATE_ERR_CHANNEL_BUSY) before the
 * attempt is counted as spent. Bounds the wait on a permanently jammed
 * channel: 8 defers at 250-999ms each is under 8s per attempt, so a
 * receipt still terminates on roughly the timescale it does today. */
#define RECEIPT_MAX_DEFERS 8u
#define RECENT_BROADCAST_RING_SIZE 8
#define PROBE_SWEEP_ROUNDS 3
#define PROBE_SWEEP_INTERVAL_MS 350
#define PROBE_COLLECTION_WINDOW_MS 5000

/* Queued messages waiting for route discovery (QUEUE_REASON_ROUTE) or for a
 * DM session to establish (QUEUE_REASON_SESSION). Both reasons share one
 * array; `reason` disambiguates. */
#define MAX_QUEUED_MSGS 8
#define QUEUE_REASON_ROUTE 0
#define QUEUE_REASON_SESSION 1
/* Covers the full handshake retransmit budget plus margin. */
#define DM_QUEUE_TTL_MS 150000

#define DM_HS_DEDUP_MAX 16
#define HANDSHAKE_WORK_QUEUE_LEN 8
#define DM_HANDSHAKE_WORKER_STACK 8192
#define DM_HANDSHAKE_WORKER_PRIORITY (MESH_TASK_PRIORITY - 2)

#define RREQ_FWD_QUEUE_CAPACITY 8

/* Give-side holder validation for s_dm_mutex. A FreeRTOS mutex MUST be given
 * by the task that took it; a cross-task or double give corrupts priority
 * inheritance bookkeeping. This macro logs file:line the moment any give runs
 * on a task that is not the recorded holder, then still gives so behavior is
 * otherwise unchanged. TAG is the per-module log tag. */
#define DM_MUTEX_GIVE()                                                                            \
    do {                                                                                           \
        TaskHandle_t holder__ = xSemaphoreGetMutexHolder(s_dm_mutex);                              \
        if (holder__ != xTaskGetCurrentTaskHandle()) {                                             \
            ESP_LOGE(TAG, "DM MUTEX MISUSE at %s:%d: give by '%s' but holder is '%s'", __FILE__,   \
                     __LINE__, pcTaskGetName(NULL), holder__ ? pcTaskGetName(holder__) : "NONE");  \
        }                                                                                          \
        xSemaphoreGive(s_dm_mutex);                                                                \
    } while (0)

/* ── Shared types ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t data[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t len;
    int16_t rssi;
    int8_t snr;
} rx_packet_t;

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
    /* Consecutive channel-busy deferrals of the CURRENT attempt (see
     * RECEIPT_MAX_DEFERS). Reset whenever the attempt is finally spent. */
    uint8_t defers;
    uint32_t due_at_ms;
} pending_receipt_t;

typedef struct {
    uint32_t dest_addr;
    uint8_t data[BRAMBLE_MAX_PACKET_SIZE];
    size_t len;
    uint32_t timestamp;
    bool used;
    uint8_t reason;      /* QUEUE_REASON_ROUTE or QUEUE_REASON_SESSION */
    uint32_t pkt_id;     /* tracking id surfaced to the caller/UI; only meaningful for
                            QUEUE_REASON_SESSION */
    uint32_t uid;        /* msg_store uid of the ONE row this user message owns. */
    int16_t channel_idx; /* transport channel a queued QUEUE_REASON_SESSION DM rode on. */
} queued_msg_t;

typedef struct {
    uint32_t src_addr;
    uint32_t eph_pub_hash;
    uint16_t ke_epoch;
    uint32_t seen_ms;
    bool used;
} dm_hs_dedup_entry_t;

typedef struct {
    uint32_t peer_addr;
    uint8_t eph_priv[32];
    uint8_t eph_pub[32];
    bool used;
} dm_pending_eph_t;

typedef struct {
    uint32_t src_addr;
    int channel_idx;
    bool initiate;
    bramble_key_exchange_t msg;
    bool have_pin;
    uint8_t pinned_x25519[32];
    uint16_t rekey_epoch;
} dm_handshake_work_item_t;

typedef struct {
    bool used;
    uint32_t due_at_ms;
    bramble_rreq_t rreq;
} pending_rreq_fwd_t;

/* ── Shared state (defined in mesh_task.c) ──────────────────────────── */

extern bramble_identity_t* s_identity;
extern uint8_t s_beacon_key[BRAMBLE_KEY_SIZE];
extern neighbor_table_t s_neighbors;
/* Parked-message sweep schedule. Mesh task only: the sweep writes it from the
 * maintenance tick and the beacon handler reads it, both on that task. */
extern parked_sweep_t s_parked_sweep;
extern dedup_buffer_t s_dedup;
extern dedup_buffer_t s_flood_dedup;
extern replay_table_t s_replay;
extern replay_table_t s_control_replay;
extern identity_store_t s_identity_pins;
extern rreq_fwd_limiter_t s_rreq_fwd_rl;
extern SemaphoreHandle_t s_state_mutex;
extern SemaphoreHandle_t s_delivery_event_mutex;
extern SemaphoreHandle_t s_nonce_mutex;
extern SemaphoreHandle_t s_dm_mutex;
extern dm_table_t* s_dm_table;
extern QueueHandle_t s_mesh_event_queue;
extern mesh_shared_state_t s_shared;
extern pending_receipt_t s_receipt_queue[RECEIPT_QUEUE_CAPACITY];
extern esp_timer_handle_t s_receipt_timer;
extern pending_probe_reply_t s_probe_reply_queue[PROBE_REPLY_QUEUE_CAPACITY];
extern esp_timer_handle_t s_probe_reply_timer;
extern delivery_event_ring_t* s_delivery_event_ring;
extern char s_node_name[BRAMBLE_NODE_NAME_MAX + 1];
extern routing_table_t s_routes;
extern rreq_dedup_t s_rreq_dedup;
extern reverse_route_table_t s_reverse_routes;
extern pending_discovery_table_t s_pending_disc;
extern queued_msg_t s_queued_msgs[MAX_QUEUED_MSGS];
extern dm_hs_dedup_entry_t s_hs_dedup[DM_HS_DEDUP_MAX];
extern dm_pending_eph_t s_pending_eph[DM_MAX_HANDSHAKING];
extern QueueHandle_t s_handshake_work_q;
extern pending_flood_relay_t s_flood_relay_queue[FLOOD_RELAY_QUEUE_CAPACITY];
extern uint32_t s_flood_relay_drops;
extern probe_ingress_limiter_t s_probe_ingress;
extern pending_rreq_fwd_t s_rreq_fwd_queue[RREQ_FWD_QUEUE_CAPACITY];
extern pending_ack_table_t s_pending_acks;
extern timesync_state_t s_timesync;
extern uint32_t s_attestation_last_ms;
extern uint32_t s_attestation_wait_ms;
extern beacon_policy_config_t s_beacon_policy;
extern beacon_policy_status_t s_beacon_status;
extern churn_sample_t s_churn_history[MAX_CHURN_HISTORY];
extern int s_churn_history_idx;
extern bramble_channel_t s_channels[MAX_CHANNELS];
extern char s_channel_names[MAX_CHANNELS][20];
extern bool s_channel_has_psk[MAX_CHANNELS];
extern int s_num_channels;
extern int s_default_channel_idx;
extern uint32_t s_last_broadcast_id;
extern uint32_t s_recent_broadcast_ids[RECENT_BROADCAST_RING_SIZE];
extern int s_recent_broadcast_idx;
extern broadcast_telemetry_mode_t s_broadcast_telemetry_mode;
extern bool s_mailbox_enabled;
extern mailbox_t s_mailbox;
extern bool s_flood_transport;
extern uint8_t s_flood_hop_limit;
extern uint32_t s_location_last_policy_tick_ms;
extern uint32_t s_location_last_send_ms;
extern location_manager_t s_location_mgr;
extern uint32_t s_probe_id;
extern uint32_t s_probe_sent_ms;
extern bool s_probe_collecting;
extern bool s_probe_complete_emitted;
extern probe_result_t s_probe_results[MAX_PROBE_RESULTS];
extern int s_probe_result_count;
extern uint8_t s_probe_rounds_sent;
extern uint32_t s_probe_next_round_ms;
extern bool s_probe_request_pending;
extern uint32_t s_probe_request_id;

/* ── Orchestration core (mesh_task.c) ───────────────────────────────── */

uint32_t now_ms(void);

/* Record that we heard addr transmit directly, on a frame that is not a
 * beacon. Refreshes last_heard/rssi/snr for an existing neighbor (see
 * neighbor_touch) and republishes the snapshot the UI and RPC read. Callers
 * must pass an address the frame's own authentication covers, never a
 * relay-mutable hint. */
void mesh_note_peer_heard(uint32_t addr, int16_t rssi, int8_t snr);

/* Admit or refresh a neighbor and store its name, under s_state_mutex.
 * s_neighbors has cross-task readers, so every write to it happens under that
 * mutex rather than touching the table directly; see the comment on the
 * wrappers in mesh_task.c for why the mutex is safe to take on the RX path.
 * Returns the entry index, or negative if there is none. */
int mesh_neighbor_update_locked(uint32_t addr, int8_t rssi, int8_t snr, uint32_t pubkey_hash,
                                uint32_t t, const char* name, uint8_t name_len);

/* The parked-retry pair, under the same mutex and for the same reason: both
 * write a neighbor entry's retry deadline. Kept as two calls because
 * mesh_flush_parked_for runs between them and transmits, so it stays outside
 * every lock. Mesh task only. */
bool mesh_parked_retry_decide_flush_locked(uint32_t peer_addr, bool is_new_peer, uint32_t t);
void mesh_parked_retry_flushed_locked(uint32_t peer_addr, int found, uint32_t t);
uint32_t next_packet_id(void);
int mesh_tx(const uint8_t* buf, uint8_t len, tx_kind_t kind);
uint32_t send_data_packet(uint32_t dest_addr, const uint8_t* payload, size_t payload_len,
                          const bramble_channel_t* ch, uint8_t app_type);
void flush_queued_messages(uint32_t dest_addr);

/* ── mesh_beacon.c ──────────────────────────────────────────────────── */

int send_beacon(void);
void attempt_identity_attestation(uint32_t t);
int control_seq_next(uint64_t* out);
bool control_replay_ok(uint32_t signer_addr, uint64_t seq);
void handle_beacon(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void handle_identity_attestation(const uint8_t* data, uint8_t len);
void record_churn_event(uint32_t t, uint8_t neighbor_count);
uint32_t emu_beacon_interval_override_ms(void);
uint32_t compute_adaptive_beacon_interval(uint32_t t, uint8_t neighbor_count);

/* ── mesh_reliability.c ─────────────────────────────────────────────── */

void mesh_receipt_timer_cb(void* arg);
void send_ack(uint32_t dest_addr, uint32_t ack_packet_id, int8_t rssi);
void handle_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void handle_delivery_receipt(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void queue_broadcast_delivery_receipt(uint32_t original_src_addr, uint32_t original_packet_id);
void mesh_process_receipt_tx_event(void);
void recent_broadcast_record(uint32_t packet_id);
void maybe_emit_implicit_broadcast_delivery(const bramble_header_t* header, const rx_packet_t* pkt);

/* ── mesh_probe.c ───────────────────────────────────────────────────── */

void mesh_process_probe_reply_tx_event(void);
void mesh_probe_reply_timer_cb(void* arg);
int mesh_send_probe_round(uint32_t pid, uint8_t round);
void mesh_start_probe_sweep(uint32_t pid);
void handle_probe(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void handle_probe_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);

/* ── mesh_dm.c ──────────────────────────────────────────────────────── */

void handle_ke_envelope(uint32_t src_addr, int channel_idx, const uint8_t* data, size_t data_len);
void handshake_worker_task(void* arg);
void maybe_schedule_dm_epoch_rekey(uint32_t t);
void maybe_trigger_dm_rehandshake(uint32_t peer);
uint32_t mesh_send_dm(int channel_idx, uint32_t dest_addr, const uint8_t* data, size_t len,
                      uint32_t uid);

/* ── mesh_routing.c ─────────────────────────────────────────────────── */

void send_rreq(const bramble_rreq_t* rreq);
void schedule_flood_relay(const uint8_t* buf, uint8_t len, uint32_t jitter_ms, uint32_t flood_key,
                          tx_kind_t tx_kind);
void process_rreq_forward_queue(uint32_t t);
void process_flood_relay_queue(uint32_t t);
void handle_rreq(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void handle_rrep(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void handle_rerr(const uint8_t* data, uint8_t len);
void rerr_fastfail_notify(uint32_t packet_id, const char* reason);
void forward_data_packet(const uint8_t* data, uint8_t len, const bramble_header_t* header);

/* ── mesh_location.c ────────────────────────────────────────────────── */

void handle_location(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr);
void mesh_location_policy_tick(uint32_t t);

/* ── mesh_mailbox.c ─────────────────────────────────────────────────── */

bool mesh_mailbox_store(uint32_t src_addr, uint32_t dest_addr, const uint8_t* raw, uint8_t raw_len,
                        uint32_t packet_id);
void mailbox_flush_for(uint32_t dest_addr);
void mailbox_expire(uint32_t t);

/* ── mesh_persist.c ─────────────────────────────────────────────────── */

int mesh_nonce_read(uint64_t* ceiling_out, void* ctx);
int mesh_nonce_write(uint64_t ceiling, void* ctx);
void mesh_pin_store_save(void);
void mesh_pin_store_load(void);
void mesh_replay_store_save(bool force);
void mesh_replay_store_load(void);
void mesh_peer_location_restore(void);
void mesh_load_network_key(void);

/* ── mesh_channels.c ────────────────────────────────────────────────── */

void mesh_load_channel_psk_flags(void);

#endif /* MESH_INTERNAL_H */
