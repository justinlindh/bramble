#ifndef SIM_NODE_H
#define SIM_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"

#define MAX_NODES 64
#define NODE_ID_LEN 16

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

    /* Statistics */
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_forwarded;
    uint64_t packets_originated;
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

#endif /* SIM_NODE_H */
