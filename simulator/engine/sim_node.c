#include "sim_node.h"
#include "../../components/packet/include/packet.h"
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include <string.h>

void node_array_init(node_array_t* array) { memset(array, 0, sizeof(*array)); }

int node_array_add(node_array_t* array, const char* id, uint32_t addr, float x, float y) {
    if (array->count >= MAX_NODES)
        return -1;

    sim_node_t* node = &array->nodes[array->count];
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

sim_node_t* node_array_find_by_id(node_array_t* array, const char* id) {
    for (int i = 0; i < array->count; i++) {
        if (strcmp(array->nodes[i].id, id) == 0)
            return &array->nodes[i];
    }
    return NULL;
}

sim_node_t* node_array_find_by_addr(node_array_t* array, uint32_t addr) {
    for (int i = 0; i < array->count; i++) {
        if (array->nodes[i].addr == addr)
            return &array->nodes[i];
    }
    return NULL;
}

sim_node_t* node_array_get(node_array_t* array, int index) {
    if (index < 0 || index >= array->count)
        return NULL;
    return &array->nodes[index];
}

void node_activate(sim_node_t* node) {
    node->active = true;
    /* Clear routing state — simulates fresh boot */
    route_init(&node->routes);
    neighbor_init(&node->neighbors);
    reverse_route_init(&node->reverse_routes);
    rreq_dedup_init(&node->rreq_dedup);
    discovery_init(&node->pending_discoveries);
    pending_ack_init(&node->pending_acks);
    dedup_init(&node->dedup);
    airtime_budget_init(&node->airtime, 0);
    reassembly_init(&node->reassembly);
    /* Crypto: generate identity but keep sim-defined address */
    {
        uint32_t saved_addr = node->addr;
        crypto_generate_identity(&node->identity);
        node->identity.address = saved_addr;
        node->identity.pubkey_hash = saved_addr; /* sim simplification */
    }
    node->crypto_counter = 0;
    node->packets_forwarded = 0;

    /* Initialize adaptive beacon controller */
    node->adaptive_beacon_interval_us = NODE_BEACON_INTERVAL_BASE_US;
    memset(node->neighbor_history, 0, sizeof(node->neighbor_history));
    node->neighbor_history_idx = 0;
    node->last_mode_transition_us = 0;
    node->adaptive_enabled = true; /* Enable by default for simulation */

    /* Per-node jitter PRNG (deterministic per address) and first beacon due
     * time: real nodes boot at uncorrelated times, so the first beacon gets
     * a random phase within one base interval instead of the tick stagger. */
    pcg32_seed(&node->beacon_rng, 0x9E3779B97F4A7C15ULL ^ node->addr);
    node->next_beacon_due_us =
        pcg32_range(&node->beacon_rng, 0, (uint32_t)(NODE_BEACON_INTERVAL_BASE_US / 1000ULL)) *
        1000ULL;
}

void node_deactivate(sim_node_t* node) { node->active = false; }

void node_move(sim_node_t* node, float x, float y) {
    node->x = x;
    node->y = y;
}

/*
 * Adaptive beacon controller — computes beacon interval based on local mesh conditions.
 * Policy:
 *  - Small/stable mesh (neighbor_count < 10, low churn) → 60s interval (conservative airtime)
 *  - Dense mesh (neighbor_count >= 10) → 60s interval (backoff to reduce collisions)
 *  - High churn detected → 15s interval (fast discovery)
 * Returns: beacon_interval_us
 */
static uint64_t adaptive_beacon_interval(sim_node_t* node, uint64_t now_us) {
    if (!node->adaptive_enabled) {
        return NODE_BEACON_INTERVAL_BASE_US;
    }

    uint8_t num_neighbors = (uint8_t)neighbor_count(&node->neighbors);

    /* Update neighbor history for churn detection */
    node->neighbor_history[node->neighbor_history_idx] = num_neighbors;
    node->neighbor_history_idx = (node->neighbor_history_idx + 1) % ADAPTIVE_NEIGHBOR_CHURN_WINDOW;

    /* Detect churn: count significant neighbor changes in rolling window */
    uint8_t churn_events = 0;
    for (int i = 1; i < ADAPTIVE_NEIGHBOR_CHURN_WINDOW; i++) {
        int prev_idx = (node->neighbor_history_idx - i + ADAPTIVE_NEIGHBOR_CHURN_WINDOW) %
                       ADAPTIVE_NEIGHBOR_CHURN_WINDOW;
        int curr_idx = (node->neighbor_history_idx - i + 1 + ADAPTIVE_NEIGHBOR_CHURN_WINDOW) %
                       ADAPTIVE_NEIGHBOR_CHURN_WINDOW;
        uint8_t prev = node->neighbor_history[prev_idx];
        uint8_t curr = node->neighbor_history[curr_idx];
        if (prev > curr ? (prev - curr) >= 2 : (curr - prev) >= 2) {
            churn_events++;
        }
    }

    bool dense = (num_neighbors >= ADAPTIVE_NEIGHBOR_DENSE_THRESHOLD);
    bool high_churn = (churn_events >= ADAPTIVE_CHURN_THRESHOLD);

    /* Apply hysteresis with cooldown. Branch mapping mirrors the firmware
     * policy (main/mesh_task.c beacon_policy): dense backs off to the max
     * interval, churn speeds up to the min interval, otherwise base. */
    uint64_t new_interval;
    if (dense) {
        new_interval = NODE_BEACON_INTERVAL_DENSE_US;
    } else if (high_churn) {
        new_interval = NODE_BEACON_INTERVAL_CHURN_US;
    } else {
        new_interval = NODE_BEACON_INTERVAL_BASE_US;
    }

    /* Only transition if cooldown elapsed */
    if (new_interval != node->adaptive_beacon_interval_us) {
        if (now_us - node->last_mode_transition_us >= ADAPTIVE_MODE_COOLDOWN_US) {
            node->last_mode_transition_us = now_us;
            node->adaptive_beacon_interval_us = new_interval;
        }
    }

    return node->adaptive_beacon_interval_us;
}

/*
 * node_tick — called every NODE_TICK_INTERVAL_US for each active node.
 * Performs: beacon TX, neighbor purge, route maintenance, discovery retry.
 * Produces outbound packets in `result` for the caller to radio-broadcast.
 */
void node_tick(sim_node_t* node, uint64_t now_us, node_tick_result_t* result) {
    result->count = 0;
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* 1. Beacon transmission with adaptive interval and firmware-style
     * per-beacon jitter (+-5 s, BEACON_JITTER_MS). Without jitter, beacon
     * phases lock to the deterministic tick stagger and identical collision
     * storms repeat every interval, which real fleets do not exhibit. */
    uint64_t beacon_interval = adaptive_beacon_interval(node, now_us);
    if (now_us >= node->next_beacon_due_us) {
        uint64_t jitter_span_ms = (uint32_t)(2 * NODE_BEACON_JITTER_US / 1000ULL);
        uint64_t jitter_us =
            (uint64_t)pcg32_range(&node->beacon_rng, 0, (uint32_t)jitter_span_ms) * 1000ULL;
        node->next_beacon_due_us = now_us + beacon_interval - NODE_BEACON_JITTER_US + jitter_us;
        node->last_beacon_us = now_us;
        node->uptime_min++;

        /*
         * Build beacon directly (without calling beacon_build() to avoid
         * pulling in the crypto dependency from beacon.c).
         */
        bramble_beacon_t beacon;
        memset(&beacon, 0, sizeof(beacon));
        beacon.header.version = BRAMBLE_VERSION;
        beacon.header.type = PKT_TYPE_BEACON;
        beacon.header.flags = 0;
        beacon.header.hop_limit = 1;          /* beacons are single-hop */
        beacon.header.dest_addr = 0xFFFFFFFF; /* broadcast */
        beacon.header.packet_id = 0;
        beacon.src_addr = node->addr;
        beacon.pubkey_hash = node->addr; /* sim simplification */
        beacon.uptime_min = node->uptime_min;
        beacon.battery_pct = 100;
        beacon.tx_queue_depth = 0;
        beacon.neighbor_count = (uint8_t)neighbor_count(&node->neighbors);
        beacon.flags = 0;
        beacon.network_time = now_ms;
        beacon.time_confidence = 0;

        outbound_packet_t* out = &result->pkts[result->count++];
        bramble_beacon_serialize(&beacon, out->data, BEACON_SIZE);
        out->len = BEACON_SIZE;
        out->is_broadcast = true;
        out->dest_addr = 0xFFFFFFFF;
        out->pkt_type = PKT_TYPE_BEACON;
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

        for (int i = 0;
             i < node->pending_discoveries.count && result->count < NODE_TICK_MAX_OUTBOUND; i++) {
            pending_discovery_t* d = &node->pending_discoveries.entries[i];
            if (discovery_should_retry(d, now_ms)) {
                /* Fresh query_id per retry with an expanded hop ring,
                 * mirroring firmware (DES-1/DES-2). */
                uint32_t query_id = pcg32_random(&node->beacon_rng);
                discovery_record_attempt(d, query_id, now_ms);
                bramble_rreq_t rreq =
                    rreq_build_originator(node->addr, d->dest_addr, query_id, node->addr,
                                          discovery_hop_limit_for_attempt(d->attempts));

                outbound_packet_t* out = &result->pkts[result->count++];
                bramble_rreq_serialize(&rreq, out->data, RREQ_SIZE);
                out->len = RREQ_SIZE;
                out->is_broadcast = true;
                out->dest_addr = 0xFFFFFFFF;
                out->pkt_type = PKT_TYPE_RREQ;
            }
        }
    }

    /* 5. Pending ACK tick — retransmit or expire */
    pending_ack_tick(&node->pending_acks, now_ms);

    /* 6. Dedup purge */
    dedup_purge(&node->dedup, now_ms);

    /* 7. Airtime budget refill */
    airtime_budget_refill(&node->airtime, now_ms);

    /* 8. Fragment reassembly purge */
    reassembly_purge(&node->reassembly, now_ms);
}
