#ifndef SIM_NODE_H
#define SIM_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include "../../components/reliability/include/reliability.h"
#include "../../components/dedup/include/dedup.h"
#include "../../components/airtime/include/airtime_budget.h"
#include "../../components/fragment/include/fragment.h"
#include "../../components/crypto/include/crypto.h"
#include "../../components/security/include/security.h"
#include "sim_random.h"

/* Forward declaration only: the full definition lives in sim_radio.h, which
 * itself includes this header, so it cannot be included back here. node_tick
 * needs just a pointer, to gate beacon/RREQ transmissions through the real
 * airtime budget using the real radio ToA (radio_frame_airtime_ms). */
typedef struct radio_config radio_config_t;

#define MAX_NODES 256
#define NODE_ID_LEN 16

/* Tick intervals (microseconds). Beacon cadence mirrors the firmware's
 * beacon policy defaults (main/mesh_task.c): base 60 s, churn min 30 s,
 * dense max 120 s, +-5 s per-beacon jitter (BEACON_JITTER_MS). The firmware
 * default is FIXED 60 s; the adaptive policy is opt-in there, and the sim
 * keeps it enabled to exercise it, with firmware-matching constants. */
#define NODE_BEACON_INTERVAL_BASE_US 60000000ULL   /* 60 s firmware base */
#define NODE_BEACON_INTERVAL_CHURN_US 30000000ULL  /* 30 s firmware churn min */
#define NODE_BEACON_INTERVAL_DENSE_US 120000000ULL /* 120 s firmware dense max */
#define NODE_BEACON_JITTER_US 5000000ULL           /* +-5 s firmware BEACON_JITTER_MS */
#define NODE_NEIGHBOR_PURGE_US 60000000ULL         /* 60 s */
#define NODE_ROUTE_MAINT_US 60000000ULL            /* 60 s */
#define NODE_DISCOVERY_CHECK_US 5000000ULL         /*  5 s */
#define NODE_TICK_INTERVAL_US 1000000ULL           /*  1 s base tick */

/* Adaptive beacon policy thresholds */
#define ADAPTIVE_NEIGHBOR_DENSE_THRESHOLD 10   /* Dense mesh if neighbor_count >= this */
#define ADAPTIVE_NEIGHBOR_CHURN_WINDOW 5       /* Track churn over last N ticks (5s) */
#define ADAPTIVE_CHURN_THRESHOLD 3             /* High churn if >=3 neighbor changes in window */
#define ADAPTIVE_MODE_COOLDOWN_US 120000000ULL /* 2 min cooldown before mode transitions */

/* Maximum packets a single tick can produce */
#define NODE_TICK_MAX_OUTBOUND 4

/* Outbound packet produced by node_tick() */
typedef struct {
    uint8_t data[256];
    uint16_t len;
    bool is_broadcast;  /* false = unicast to dest_addr */
    uint32_t dest_addr; /* ignored if is_broadcast */
    uint8_t pkt_type;   /* PKT_TYPE_* for emitter */
} outbound_packet_t;

/* Result container for node_tick() */
typedef struct {
    outbound_packet_t pkts[NODE_TICK_MAX_OUTBOUND];
    int count;
} node_tick_result_t;

/* Virtual node state */
typedef struct {
    char id[NODE_ID_LEN];
    uint32_t addr;
    float x;
    float y;
    bool active;

    /* Bramble protocol state */
    routing_table_t routes;
    neighbor_table_t neighbors;
    reverse_route_table_t reverse_routes;
    rreq_dedup_t rreq_dedup;
    pending_discovery_table_t pending_discoveries;

    /* Reliability state */
    pending_ack_table_t pending_acks;

    /* Dedup state */
    dedup_buffer_t dedup;

    /* Airtime budget */
    airtime_budget_t airtime;

    /* RREQ rate limiters (components/security), same instances mesh_task_start
     * initializes: rreq_rate gates fresh discovery origination (per
     * neighbor/dest, 30s), rreq_fwd gates this node's aggregate forwarded-RREQ
     * rate (global token bucket, BURST 16 / 2s refill). */
    rreq_rate_limiter_t rreq_rate;
    rreq_fwd_limiter_t rreq_fwd;
    uint32_t rreq_rate_denied;
    uint32_t rreq_fwd_denied;

    /* Fragment reassembly */
    reassembly_ctx_t reassembly;

    /* Crypto identity */
    bramble_identity_t identity;
    uint32_t crypto_counter; /* nonce counter for encryption */

    /* Per-node tick state */
    uint32_t tick_seq;
    uint64_t last_beacon_us;
    uint64_t last_neighbor_purge_us;
    uint64_t last_route_maint_us;
    uint64_t last_discovery_check_us;
    uint32_t uptime_min;

    /* Adaptive beacon controller state */
    uint64_t adaptive_beacon_interval_us; /* Current dynamic beacon interval */
    uint8_t
        neighbor_history[ADAPTIVE_NEIGHBOR_CHURN_WINDOW]; /* Rolling window of neighbor counts */
    uint8_t neighbor_history_idx;                         /* Current index in rolling window */
    uint64_t last_mode_transition_us;                     /* When did we last change mode? */
    bool adaptive_enabled;                                /* Feature flag for adaptive policy */
    uint64_t next_beacon_due_us; /* Jittered absolute due time of the next beacon */
    pcg32_state_t beacon_rng;    /* Per-node PRNG for beacon jitter (seeded from addr) */

    /* Half-duplex radio state: end of this node's in-progress transmission */
    uint64_t tx_busy_until_us;

    /* Statistics */
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_forwarded;
    uint64_t packets_originated;
    uint64_t beacons_sent;
    uint64_t airtime_tx_us; /* cumulative real time-on-air transmitted */

    /* Airtime budget denials, by AIRTIME_IDX_* lane. Incremented at every
     * gated TX site when airtime_budget_can_transmit refuses; the packet is
     * dropped (no queue), matching firmware's tx_gate drop-no-queue semantics. */
    uint32_t budget_denied[AIRTIME_TIER_COUNT];
} sim_node_t;

typedef struct {
    sim_node_t nodes[MAX_NODES];
    int count;
} node_array_t;

void node_array_init(node_array_t* array);
int node_array_add(node_array_t* array, const char* id, uint32_t addr, float x, float y);
sim_node_t* node_array_find_by_id(node_array_t* array, const char* id);
sim_node_t* node_array_find_by_addr(node_array_t* array, uint32_t addr);
sim_node_t* node_array_get(node_array_t* array, int index);

void node_activate(sim_node_t* node);
void node_deactivate(sim_node_t* node);
void node_move(sim_node_t* node, float x, float y);
void node_tick(sim_node_t* node, uint64_t now_us, const radio_config_t* radio,
               node_tick_result_t* result);

#endif /* SIM_NODE_H */
