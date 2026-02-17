#include "sim_node.h"
#include "../../components/packet/include/packet.h"
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

/*
 * node_tick — called every NODE_TICK_INTERVAL_US for each active node.
 * Performs: beacon TX, neighbor purge, route maintenance, discovery retry.
 * Produces outbound packets in `result` for the caller to radio-broadcast.
 */
void node_tick(sim_node_t *node, uint64_t now_us, node_tick_result_t *result) {
    result->count = 0;
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* 1. Beacon transmission */
    if (now_us - node->last_beacon_us >= NODE_BEACON_INTERVAL_US) {
        node->last_beacon_us = now_us;
        node->uptime_min++;

        /*
         * Build beacon directly (without calling beacon_build() to avoid
         * pulling in the crypto dependency from beacon.c).
         */
        bramble_beacon_t beacon;
        memset(&beacon, 0, sizeof(beacon));
        beacon.header.version   = BRAMBLE_VERSION;
        beacon.header.type      = PKT_TYPE_BEACON;
        beacon.header.flags     = 0;
        beacon.header.hop_limit = 1;               /* beacons are single-hop */
        beacon.header.dest_addr = 0xFFFFFFFF;      /* broadcast */
        beacon.header.packet_id = 0;
        beacon.src_addr         = node->addr;
        beacon.pubkey_hash      = node->addr;      /* sim simplification */
        beacon.uptime_min       = node->uptime_min;
        beacon.battery_pct      = 100;
        beacon.tx_queue_depth   = 0;
        beacon.neighbor_count   = (uint8_t)neighbor_count(&node->neighbors);
        beacon.flags            = 0;
        beacon.network_time     = now_ms;
        beacon.time_confidence  = 0;

        outbound_packet_t *out = &result->pkts[result->count++];
        bramble_beacon_serialize(&beacon, out->data, BEACON_SIZE);
        out->len          = BEACON_SIZE;
        out->is_broadcast = true;
        out->dest_addr    = 0xFFFFFFFF;
        out->pkt_type     = PKT_TYPE_BEACON;
        node->beacons_sent++;
    }

    /* 2. Neighbor table purge */
    if (now_us - node->last_neighbor_purge_us >= NODE_NEIGHBOR_PURGE_US) {
        node->last_neighbor_purge_us = now_us;
        neighbor_purge(&node->neighbors, now_ms);
    }

    /* 3. Route maintenance */
    if (now_us - node->last_route_maint_us >= NODE_ROUTE_MAINT_US) {
        node->last_route_maint_us = now_us;
        route_maintenance(&node->routes, now_ms);
    }

    /* 4. Discovery retry check */
    if (now_us - node->last_discovery_check_us >= NODE_DISCOVERY_CHECK_US) {
        node->last_discovery_check_us = now_us;

        for (int i = 0; i < node->pending_discoveries.count &&
                        result->count < NODE_TICK_MAX_OUTBOUND; i++) {
            pending_discovery_t *d = &node->pending_discoveries.entries[i];
            if (discovery_should_retry(d, now_ms)) {
                uint32_t query_id = d->query_id;
                bramble_rreq_t rreq = rreq_build_originator(
                    node->addr, d->dest_addr, query_id, node->addr);
                discovery_record_attempt(d, now_ms);

                outbound_packet_t *out = &result->pkts[result->count++];
                bramble_rreq_serialize(&rreq, out->data, RREQ_SIZE);
                out->len          = RREQ_SIZE;
                out->is_broadcast = true;
                out->dest_addr    = 0xFFFFFFFF;
                out->pkt_type     = PKT_TYPE_RREQ;
            }
        }
    }
}
