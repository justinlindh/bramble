#ifndef SIM_NODE_H
#define SIM_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"

#define MAX_NODES 256
#define NODE_ID_LEN 16

/* Tick intervals (microseconds) */
#define NODE_BEACON_INTERVAL_US     5000000ULL    /* 5 s — faster than real firmware for sim responsiveness */
#define NODE_NEIGHBOR_PURGE_US      60000000ULL   /* 60 s */
#define NODE_ROUTE_MAINT_US         60000000ULL   /* 60 s */
#define NODE_DISCOVERY_CHECK_US      5000000ULL   /*  5 s */
#define NODE_TICK_INTERVAL_US        1000000ULL   /*  1 s base tick */

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

    /* Per-node tick state */
    uint32_t tick_seq;
    uint64_t last_beacon_us;
    uint64_t last_neighbor_purge_us;
    uint64_t last_route_maint_us;
    uint64_t last_discovery_check_us;
    uint32_t uptime_min;

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
