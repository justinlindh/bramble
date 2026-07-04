#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ─── Include simulator + protocol headers ─────────────────────────────── */
#include "../engine/sim_event.h"
#include "../engine/sim_node.h"
#include "../engine/sim_radio.h"
#include "../engine/sim_random.h"
#include "../engine/sim_scenario.h"
#include "../engine/sim_metrics.h"
#include "../engine/sim_anomaly.h"
#include "../engine/sim_emitter.h"

/* ─── New component headers (Phase 6) ──────────────────────────────────── */
#include "../../components/mailbox/include/mailbox.h"
#include "../../components/location/include/location.h"
/* public_channel.h includes channel_key.h which includes crypto.h (via -I) */
#include "../../components/channel/include/channel_key.h"
#include "../../components/channel/include/public_channel.h"

/* ─── Extended per-node state (new components) ──────────────────────────── */
typedef struct {
    mailbox_t mailbox;           /* store-and-forward for offline destinations */
    location_manager_t location; /* position sharing manager */
    bool initialized;
} bridge_node_ext_t;

/* ─── Extended bridge-level metrics ────────────────────────────────────── */
typedef struct {
    uint64_t mailbox_stored;    /* DATA packets stored for offline dest */
    uint64_t mailbox_delivered; /* stored packets delivered on node rejoin */
    uint64_t mailbox_expired;   /* mailbox entries expired (24h TTL) */
    uint64_t location_updates;  /* location position updates processed */
} bridge_ext_metrics_t;

/* Accessor functions */
bridge_node_ext_t* bridge_node_ext_get(int node_idx);
bridge_ext_metrics_t* bridge_ext_metrics_get(void);
void bridge_node_ext_init_all(void);

/*
 * bridge_handle_node_join_ext:
 *   Called after a node joins the simulation to initialize its extended state
 *   (set simulated position, assign simulated group membership, etc.)
 */
void bridge_handle_node_join_ext(int node_idx, uint32_t addr, float x, float y, uint64_t now_us);

/*
 * bridge_apply_duty_cycle_cap:
 *   Applies the scenario's optional regulatory duty-cycle cap (DES-8) to one
 *   node's real airtime budget via airtime_budget_set_duty_cap, exactly as
 *   firmware's mesh_task_start -> tx_gate_global_init -> tx_gate_init wiring
 *   does. Call after every node_activate (initial load, scripted/chaos
 *   join, RPC add-node): node_activate's airtime_budget_init resets the cap,
 *   so it must be reapplied each time. No-op call site convention: caller
 *   only calls this when the scenario's radio.duty_cycle_set is true.
 */
void bridge_apply_duty_cycle_cap(sim_node_t* node, uint8_t max_duty_cycle_pct);

/* ─── Global simulation time ───────────────────────────────────────────── */
extern uint64_t g_bridge_sim_time_us;
void bridge_set_sim_time(uint64_t us);

/* Bramble's time source — defined in bridge.c */
uint32_t sim_get_time_ms(void);

/* ─── Event union accessors (cgo cannot access C unions) ───────────────── */
node_event_data_t bridge_get_node_event(const sim_event_t* e);
packet_event_data_t bridge_get_packet_event(const sim_event_t* e);
tick_event_data_t bridge_get_tick_event(const sim_event_t* e);
interference_event_data_t bridge_get_interference_event(const sim_event_t* e);
event_type_t bridge_get_event_type(const sim_event_t* e);
uint64_t bridge_get_event_timestamp(const sim_event_t* e);

/* ─── Event construction helpers ───────────────────────────────────────── */
sim_event_t bridge_make_tick_event(uint64_t ts_us, const char* node_id, uint32_t tick_seq);
sim_event_t bridge_make_node_event(event_type_t type, uint64_t ts_us, const char* node_id,
                                   uint32_t addr, float x, float y);
sim_event_t bridge_make_generate_msg_event(uint64_t ts_us, const char* node_id, uint32_t dest_addr);
sim_event_t bridge_make_interference_start(uint64_t ts_us, float cx, float cy, float radius);
sim_event_t bridge_make_interference_end(uint64_t ts_us, int zone_index);
/* Test-only injection: builds an EVT_RECEIVE_PACKET event directly (normally
 * only sim_radio_broadcast produces these), so Go tests can drive
 * bridge_handle_receive_packet with a hand-built frame without going through
 * the full radio model. air_start_us/air_end_us are set to [ts_us, ts_us], so
 * radio_check_reception sees no occupancy window to overlap against. */
sim_event_t bridge_make_receive_packet_event(uint64_t ts_us, uint32_t src_addr, uint32_t dest_addr,
                                             const uint8_t* data, uint16_t len);
/* Phase 2 Task 0 (managed-flooding routing mode): see bridge.c for the full
 * comment. Used only by gosim/flood.go's own EVT_SEND_PACKET handling. */
sim_event_t bridge_make_flood_relay_event(uint64_t due_us, uint32_t node_addr, const uint8_t* frame,
                                          uint16_t len);

/* ─── Message tracking ─────────────────────────────────────────────────── */
#define MAX_MSG_TRACK 1024

typedef struct {
    uint32_t packet_id;
    uint32_t src_addr;
    uint32_t dest_addr;
    uint64_t sent_us;
    uint8_t attempt; /* retransmission count */
    bool active;
    /* Phase 2 "save reactive routing" Part A: set once the delivery
     * receipt for this packet_id has been observed back at the true
     * originator (bridge_msg_track_confirm). Tracked separately from
     * `active` because `active` already flips false the moment DATA
     * reaches the destination (bridge_msg_track_complete, called from
     * _handle_data on arrival) -- well before any receipt has traveled
     * back, often before it even exists. Reset to false whenever a slot is
     * reused for a new scripted message (bridge_msg_track_add). */
    bool confirmed;
} msg_tracker_t;

void bridge_msg_track_init(msg_tracker_t* track, int count);
int bridge_msg_track_add(msg_tracker_t* track, int count, uint32_t packet_id, uint32_t src_addr,
                         uint32_t dest_addr, uint64_t sent_us);
bool bridge_msg_track_complete(msg_tracker_t* track, int count, uint32_t packet_id, uint64_t now_us,
                               metrics_state_t* metrics);
/* Marks packet_id as confirmed (its delivery receipt reached the true
 * originator) and records it in metrics exactly once, regardless of the
 * entry's `active` state (which is normally already false by the time this
 * runs; see the struct doc comment above). Looks the entry up by packet_id
 * alone, not `active`, since track_add's dest_addr/sent_us fields on that
 * entry are still meaningful here. Returns true iff this call is the one
 * that recorded the confirmation (false if the entry was not found, or was
 * already confirmed). */
bool bridge_msg_track_confirm(msg_tracker_t* track, int count, uint32_t packet_id,
                              metrics_state_t* metrics);

/* ─── Packet handling wrappers ─────────────────────────────────────────── */
/*
 * bridge_handle_receive_packet:
 *   Dispatches a received packet event to the appropriate protocol handler
 *   (beacon, rreq, rrep, rerr, data).
 */
void bridge_handle_receive_packet(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                                  pcg32_state_t* rng, event_queue_t* events,
                                  metrics_state_t* metrics, node_anomaly_tracker_t* anomaly,
                                  msg_tracker_t* msg_track, int msg_track_count);

/*
 * bridge_handle_generate_message:
 *   Handles route discovery + DATA send logic for EVT_GENERATE_MESSAGE.
 */
void bridge_handle_generate_message(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                                    pcg32_state_t* rng, event_queue_t* events,
                                    metrics_state_t* metrics, node_anomaly_tracker_t* anomaly,
                                    msg_tracker_t* msg_track, int msg_track_count);

/*
 * bridge_handle_flood_relay:
 *   Fires a jittered channel-flood relay (Task 5) scheduled by _handle_data's
 *   broadcast branch, once its EVT_SEND_PACKET due time elapses.
 */
void bridge_handle_flood_relay(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                               pcg32_state_t* rng, event_queue_t* events, metrics_state_t* metrics);

/*
 * bridge_handle_retransmit:
 *   Checks pending_acks for entries needing retransmission and re-broadcasts.
 */
void bridge_handle_retransmit(sim_node_t* node, node_array_t* nodes, radio_config_t* radio,
                              pcg32_state_t* rng, event_queue_t* events, metrics_state_t* metrics,
                              uint64_t now_us);

/*
 * bridge_init:
 *   Initialize bridge-level state (relay path tracker, etc.)
 */
void bridge_init(void);

/*
 * Phase 2 "save reactive routing" Part B: intermediate-node RREP
 * (components/routing/include/discovery.h's rrep_build_intermediate /
 * intermediate_rrep_route_usable). Firmware (main/mesh_task.c's
 * handle_rreq) always has this on; gosim exposes a runtime on/off switch
 * purely so a scenario can A/B the SAME topology/traffic with and without
 * the feature for measurement (see internal-planning plans/2026-07-04
 * phase2-scale-framework.md's before/after requirement), without needing
 * two firmware builds. Defaults to true (the shipped firmware behavior);
 * gosim/sim.go resets this explicitly on every run (see the scenario's
 * optional "intermediate_rrep" JSON field), so no run leaks a previous
 * run's setting.
 */
void bridge_set_intermediate_rrep_enabled(bool enabled);
bool bridge_get_intermediate_rrep_enabled(void);

#endif /* BRIDGE_H */
