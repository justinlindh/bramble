#ifndef SIM_ANOMALY_H
#define SIM_ANOMALY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "sim_node.h"

/* ── Route flap ──────────────────────────────────────────────────────── */
#define MAX_ROUTE_FLAP_TRACK  64
#define ROUTE_FLAP_WINDOW_US  2000000ULL  /* 2 s */
#define ROUTE_FLAP_THRESHOLD  5

typedef struct {
    uint32_t dest_addr;
    uint32_t next_hop;
    uint64_t timestamp_us;
} route_change_t;

typedef struct {
    route_change_t changes[MAX_ROUTE_FLAP_TRACK];
    int count;
} route_flap_tracker_t;

/* ── Black hole ──────────────────────────────────────────────────────── */
#define BLACKHOLE_WINDOW_US  10000000ULL  /* 10 s */
#define BLACKHOLE_THRESHOLD  5            /* min receives before we check */

typedef struct {
    uint64_t window_start_us;
    uint32_t rx_count;          /* packets received this window */
    uint32_t fwd_count;         /* packets forwarded/delivered this window */
    bool     reported;          /* suppress duplicate anomalies per window */
} blackhole_tracker_t;

/* ── Route loop ──────────────────────────────────────────────────────── */
#define MAX_LOOP_TRACK    32
#define LOOP_TTL_US       5000000ULL  /* 5 s */

typedef struct {
    uint32_t packet_id;
    uint64_t first_seen_us;
} loop_seen_t;

typedef struct {
    loop_seen_t seen[MAX_LOOP_TRACK];
    int         count;
} loop_tracker_t;

/* ── Excessive retransmission ─────────────────────────────────────────── */
#define MAX_RREQ_TRACK    64
#define RREQ_WINDOW_US    10000000ULL  /* 10 s */
#define RREQ_THRESHOLD    6

typedef struct {
    uint32_t dest_addr;
    uint64_t timestamps[RREQ_THRESHOLD + 4];
    int      count;
} rreq_retx_entry_t;

typedef struct {
    rreq_retx_entry_t entries[MAX_RREQ_TRACK];
    int count;
} rreq_retx_tracker_t;

/* ── Combined per-node tracker ───────────────────────────────────────── */
typedef struct {
    route_flap_tracker_t  flap;
    blackhole_tracker_t   blackhole;
    loop_tracker_t        loop;
    rreq_retx_tracker_t   rreq_retx;
} node_anomaly_tracker_t;

/* ── Function declarations ───────────────────────────────────────────── */

void anomaly_init(node_anomaly_tracker_t *t);

/* Route flap: call when a route to dest changes next_hop */
bool anomaly_check_route_flap(route_flap_tracker_t *tracker,
    uint32_t dest_addr, uint32_t next_hop,
    uint64_t now_us, FILE *emit_out, const char *node_id);

/* Black hole: call on every packet receive and forward/deliver */
void anomaly_record_rx(blackhole_tracker_t *t, uint64_t now_us);
void anomaly_record_fwd(blackhole_tracker_t *t, uint64_t now_us);
bool anomaly_check_blackhole(blackhole_tracker_t *t,
    uint64_t now_us, FILE *emit_out, const char *node_id);

/* Route loop: call when a DATA packet arrives; returns true if loop detected */
bool anomaly_check_loop(loop_tracker_t *t,
    uint32_t packet_id, uint64_t now_us,
    FILE *emit_out, const char *node_id);

/* Excessive RREQ retransmission: call when a node originates/retransmits RREQ */
bool anomaly_check_rreq_retx(rreq_retx_tracker_t *t,
    uint32_t dest_addr, uint64_t now_us,
    FILE *emit_out, const char *node_id);

/* Mesh partition: call after any topology change.
 * Performs BFS and emits anomaly if graph is disconnected.
 * nodes / node_count: the active node list.
 * radio_range: used to determine adjacency (Euclidean distance).
 */
void anomaly_check_partition(
    node_array_t *nodes, float radio_range,
    uint64_t now_us, FILE *emit_out);

/* Legacy shim — used by old call sites that only passed route_flap_tracker_t */
static inline bool anomaly_check_route_flap_legacy(
    route_flap_tracker_t *tracker,
    uint32_t dest_addr, uint32_t next_hop,
    uint64_t now_us, FILE *emit_out, const char *node_id)
{
    return anomaly_check_route_flap(tracker, dest_addr, next_hop,
                                    now_us, emit_out, node_id);
}

#endif /* SIM_ANOMALY_H */
