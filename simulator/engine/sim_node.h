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

#define MAX_NODES 256
#define NODE_ID_LEN 16

/* Tick intervals (microseconds) */
#define NODE_BEACON_INTERVAL_BASE_US    15000000ULL   /* 15 s baseline */
#define NODE_BEACON_INTERVAL_STABLE_US  60000000ULL   /* 60 s for stable/large meshes */
#define NODE_NEIGHBOR_PURGE_US          60000000ULL   /* 60 s */
#define NODE_ROUTE_MAINT_US             60000000ULL   /* 60 s */
#define NODE_DISCOVERY_CHECK_US          5000000ULL   /*  5 s */
#define NODE_TICK_INTERVAL_US            1000000ULL   /*  1 s base tick */

/* Adaptive beacon policy thresholds */
#define ADAPTIVE_NEIGHBOR_DENSE_THRESHOLD  10   /* Dense mesh if neighbor_count >= this */
#define ADAPTIVE_NEIGHBOR_CHURN_WINDOW     5    /* Track churn over last N ticks (5s) */
#define ADAPTIVE_CHURN_THRESHOLD           3    /* High churn if >=3 neighbor changes in window */
#define ADAPTIVE_MODE_COOLDOWN_US      120000000ULL  /* 2 min cooldown before mode transitions */

/* Maximum packets a single tick can produce */
#define NODE_TICK_MAX_OUTBOUND 4

/* Outbound packet produced by node_tick() */
typedef struct {
    uint8_t  data[256];
    uint16_t len;
    bool     is_broadcast;   /* false = unicast to dest_addr */
    uint32_t dest_addr;      /* ignored if is_broadcast */
    uint8_t  pkt_type;       /* PKT_TYPE_* for emitter */
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
    flow_control_t flow_control;

    /* Dedup state */
    dedup_buffer_t dedup;

    /* Airtime budget */
    airtime_budget_t airtime;

    /* Fragment reassembly */
    reassembly_ctx_t reassembly;

    /* Crypto identity */
    bramble_identity_t identity;
    uint32_t crypto_counter;  /* nonce counter for encryption */

    /* Per-node tick state */
    uint32_t tick_seq;
    uint64_t last_beacon_us;
    uint64_t last_neighbor_purge_us;
    uint64_t last_route_maint_us;
    uint64_t last_discovery_check_us;
    uint32_t uptime_min;

    /* Adaptive beacon controller state */
    uint64_t adaptive_beacon_interval_us;   /* Current dynamic beacon interval */
    uint8_t  neighbor_history[ADAPTIVE_NEIGHBOR_CHURN_WINDOW]; /* Rolling window of neighbor counts */
    uint8_t  neighbor_history_idx;          /* Current index in rolling window */
    uint64_t last_mode_transition_us;       /* When did we last change mode? */
    bool     adaptive_enabled;              /* Feature flag for adaptive policy */

    /* Statistics */
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_forwarded;
    uint64_t packets_originated;
    uint64_t beacons_sent;
} sim_node_t;

typedef struct {
    sim_node_t nodes[MAX_NODES];
    int count;
} node_array_t;

void node_array_init(node_array_t *array);
int node_array_add(node_array_t *array, const char *id, uint32_t addr, float x, float y);
sim_node_t *node_array_find_by_id(node_array_t *array, const char *id);
sim_node_t *node_array_find_by_addr(node_array_t *array, uint32_t addr);
sim_node_t *node_array_get(node_array_t *array, int index);

void node_activate(sim_node_t *node);
void node_deactivate(sim_node_t *node);
void node_move(sim_node_t *node, float x, float y);
void node_tick(sim_node_t *node, uint64_t now_us, node_tick_result_t *result);

#endif /* SIM_NODE_H */
