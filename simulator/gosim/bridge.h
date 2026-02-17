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

/* ─── Global simulation time ───────────────────────────────────────────── */
extern uint64_t g_bridge_sim_time_us;
void bridge_set_sim_time(uint64_t us);

/* Bramble's time source — defined in bridge.c */
uint32_t sim_get_time_ms(void);

/* ─── Event union accessors (cgo cannot access C unions) ───────────────── */
node_event_data_t       bridge_get_node_event(const sim_event_t *e);
packet_event_data_t     bridge_get_packet_event(const sim_event_t *e);
tick_event_data_t       bridge_get_tick_event(const sim_event_t *e);
interference_event_data_t bridge_get_interference_event(const sim_event_t *e);
event_type_t            bridge_get_event_type(const sim_event_t *e);
uint64_t                bridge_get_event_timestamp(const sim_event_t *e);

/* ─── Event construction helpers ───────────────────────────────────────── */
sim_event_t bridge_make_tick_event(uint64_t ts_us, const char *node_id, uint32_t tick_seq);
sim_event_t bridge_make_node_event(event_type_t type, uint64_t ts_us,
                                    const char *node_id, uint32_t addr, float x, float y);
sim_event_t bridge_make_generate_msg_event(uint64_t ts_us, const char *node_id, uint32_t dest_addr);
sim_event_t bridge_make_interference_start(uint64_t ts_us, float cx, float cy, float radius);
sim_event_t bridge_make_interference_end(uint64_t ts_us, int zone_index);

/* ─── Message tracking ─────────────────────────────────────────────────── */
#define MAX_MSG_TRACK 1024

typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint64_t sent_us;
    bool     active;
} msg_tracker_t;

void bridge_msg_track_init(msg_tracker_t *track, int count);
int  bridge_msg_track_add(msg_tracker_t *track, int count,
                           uint32_t packet_id, uint32_t dest_addr, uint64_t sent_us);
bool bridge_msg_track_complete(msg_tracker_t *track, int count,
                                uint32_t packet_id, uint64_t now_us,
                                metrics_state_t *metrics);

/* ─── Packet handling wrappers ─────────────────────────────────────────── */
/*
 * bridge_handle_receive_packet:
 *   Dispatches a received packet event to the appropriate protocol handler
 *   (beacon, rreq, rrep, rerr, data).
 */
void bridge_handle_receive_packet(
    sim_event_t *event,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    msg_tracker_t *msg_track,
    int msg_track_count);

/*
 * bridge_handle_generate_message:
 *   Handles route discovery + DATA send logic for EVT_GENERATE_MESSAGE.
 */
void bridge_handle_generate_message(
    sim_event_t *event,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    msg_tracker_t *msg_track,
    int msg_track_count);

#endif /* BRIDGE_H */
