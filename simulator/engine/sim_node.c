#include "sim_node.h"
#include <string.h>

void node_array_init(node_array_t *array) {
    memset(array, 0, sizeof(*array));
}

int node_array_add(node_array_t *array, const char *id, uint32_t addr, float x, float y) {
    if (array->count >= MAX_NODES)
        return -1;

    sim_node_t *node = &array->nodes[array->count];
    memset(node, 0, sizeof(*node));
    strncpy(node->id, id, NODE_ID_LEN - 1);
    node->id[NODE_ID_LEN - 1] = '\0';
    node->addr = addr;
    node->x = x;
    node->y = y;
    node->active = true;

    /* Initialize Bramble state */
    route_init(&node->routes);
    neighbor_init(&node->neighbors);
    reverse_route_init(&node->reverse_routes);
    rreq_dedup_init(&node->rreq_dedup);
    discovery_init(&node->pending_discoveries);

    return array->count++;
}

sim_node_t *node_array_find_by_id(node_array_t *array, const char *id) {
    for (int i = 0; i < array->count; i++) {
        if (strcmp(array->nodes[i].id, id) == 0)
            return &array->nodes[i];
    }
    return NULL;
}

sim_node_t *node_array_find_by_addr(node_array_t *array, uint32_t addr) {
    for (int i = 0; i < array->count; i++) {
        if (array->nodes[i].addr == addr)
            return &array->nodes[i];
    }
    return NULL;
}

sim_node_t *node_array_get(node_array_t *array, int index) {
    if (index < 0 || index >= array->count)
        return NULL;
    return &array->nodes[index];
}

void node_activate(sim_node_t *node) {
    node->active = true;
}

void node_deactivate(sim_node_t *node) {
    node->active = false;
}

void node_move(sim_node_t *node, float x, float y) {
    node->x = x;
    node->y = y;
}
