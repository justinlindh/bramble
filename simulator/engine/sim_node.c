#include "sim_node.h"
#include "sim_radio.h"
#include "../../components/packet/include/packet.h"
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include <string.h>

void sim_beacon_policy_init(sim_beacon_policy_t* policy) {
    policy->adaptive = false; /* firmware default: BEACON_MODE_FIXED, disabled */
    policy->interval_ms = SIM_BEACON_POLICY_DEFAULT_INTERVAL_MS;
    policy->min_interval_ms = SIM_BEACON_POLICY_DEFAULT_MIN_MS;
    policy->max_interval_ms = SIM_BEACON_POLICY_DEFAULT_MAX_MS;
    policy->dense_threshold = SIM_BEACON_POLICY_DEFAULT_DENSE_THRESHOLD;
    policy->churn_threshold = SIM_BEACON_POLICY_DEFAULT_CHURN_THRESHOLD;
    policy->churn_window_ms = SIM_BEACON_POLICY_DEFAULT_CHURN_WINDOW_MS;
}

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
    /* Same instances mesh_task_start initializes (main/mesh_task.c:4536-4537). */
    rreq_rate_init(&node->rreq_rate);
    rreq_fwd_init(&node->rreq_fwd, 0);
    node->rreq_rate_denied = 0;
    node->rreq_fwd_denied = 0;
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
    memset(node->budget_denied, 0, sizeof(node->budget_denied));

    /* Beacon churn tracking reset (fresh boot has no history). The policy
     * CONFIG (sim_beacon_policy_t) lives outside sim_node_t entirely, one
     * shared instance passed into node_tick every call (see sim_node.h),
     * so there is nothing to reset for it here. */
    memset(node->beacon_churn_history, 0, sizeof(node->beacon_churn_history));
    node->beacon_churn_history_idx = 0;
    node->beacon_last_neighbor_count = 0;

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
 * node_tick — called every NODE_TICK_INTERVAL_US for each active node.
 * Performs: beacon TX, neighbor purge, route maintenance, discovery retry.
 * Produces outbound packets in `result` for the caller to radio-broadcast.
 */
void node_tick(sim_node_t* node, uint64_t now_us, const radio_config_t* radio,
               const sim_beacon_policy_t* beacon_policy, node_tick_result_t* result) {
    result->count = 0;
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* 1. Beacon transmission. The interval is decided by the REAL firmware
     * function beacon_interval_decide() (main/beacon_policy_calc.c),
     * evaluated every tick exactly like firmware's mesh_periodic_maintenance
     * evaluates compute_adaptive_beacon_interval every loop iteration
     * (mesh_task.c:3033-3046): the decision recomputes continuously, only
     * the ACT (actually sending) is gated on the due-time check below.
     * Firmware-style per-beacon jitter (+-5 s, BEACON_JITTER_MS) is layered
     * on top: without jitter, beacon phases lock to the deterministic tick
     * stagger and identical collision storms repeat every interval, which
     * real fleets do not exhibit. */
    uint8_t num_neighbors = (uint8_t)neighbor_count(&node->neighbors);
    if (num_neighbors != node->beacon_last_neighbor_count) {
        node->beacon_churn_history[node->beacon_churn_history_idx].timestamp = now_ms;
        node->beacon_churn_history[node->beacon_churn_history_idx].neighbor_count = num_neighbors;
        node->beacon_churn_history_idx = (node->beacon_churn_history_idx + 1) % MAX_CHURN_HISTORY;
        node->beacon_last_neighbor_count = num_neighbors;
    }
    uint8_t churn_events = beacon_churn_count(node->beacon_churn_history, MAX_CHURN_HISTORY, now_ms,
                                              beacon_policy->churn_window_ms);
    /* enabled and mode_is_adaptive collapse to one scenario flag: the sim
     * only distinguishes fixed vs adaptive, never firmware's separate
     * "adaptive mode selected but disabled" RPC nuance, which has no
     * scenario equivalent. */
    beacon_interval_decision_t beacon_decision =
        beacon_interval_decide(beacon_policy->adaptive, beacon_policy->adaptive,
                               beacon_policy->interval_ms, beacon_policy->min_interval_ms,
                               beacon_policy->max_interval_ms, beacon_policy->dense_threshold,
                               beacon_policy->churn_threshold, num_neighbors, churn_events);
    uint64_t beacon_interval = (uint64_t)beacon_decision.interval_ms * 1000ULL; /* ms -> us */
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

        /* Budget-gate the beacon exactly like firmware's mesh_tx: refresh
         * the profile's peer-count scaler from the current neighbor table,
         * then check the BROADCAST lane (tx_gate_kind_tier: TX_KIND_BEACON
         * -> AIRTIME_TIER_BROADCAST) before putting it on the air. Denied
         * beacons are dropped, not queued, matching tx_gate's no-queue
         * semantics; the next beacon is still scheduled on schedule. */
        airtime_budget_set_mesh_size(&node->airtime, (uint8_t)neighbor_count(&node->neighbors));
        uint32_t beacon_airtime_ms = radio_frame_airtime_ms(radio, BEACON_SIZE);
        if (airtime_budget_can_transmit(&node->airtime, AIRTIME_TIER_BROADCAST,
                                        beacon_airtime_ms)) {
            outbound_packet_t* out = &result->pkts[result->count++];
            bramble_beacon_serialize(&beacon, out->data, BEACON_SIZE);
            out->len = BEACON_SIZE;
            out->is_broadcast = true;
            out->dest_addr = 0xFFFFFFFF;
            out->pkt_type = PKT_TYPE_BEACON;
            node->beacons_sent++;
            airtime_budget_debit(&node->airtime, AIRTIME_TIER_BROADCAST, beacon_airtime_ms);
        } else {
            node->budget_denied[AIRTIME_IDX_BROADCAST]++;
        }
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

                /* Budget-gate the retry RREQ: tx_gate_kind_tier maps
                 * TX_KIND_ROUTING to AIRTIME_TIER_CRITICAL. */
                airtime_budget_set_mesh_size(&node->airtime,
                                             (uint8_t)neighbor_count(&node->neighbors));
                uint32_t rreq_airtime_ms = radio_frame_airtime_ms(radio, RREQ_SIZE);
                if (airtime_budget_can_transmit(&node->airtime, AIRTIME_TIER_CRITICAL,
                                                rreq_airtime_ms)) {
                    outbound_packet_t* out = &result->pkts[result->count++];
                    bramble_rreq_serialize(&rreq, out->data, RREQ_SIZE);
                    out->len = RREQ_SIZE;
                    out->is_broadcast = true;
                    out->dest_addr = 0xFFFFFFFF;
                    out->pkt_type = PKT_TYPE_RREQ;
                    airtime_budget_debit(&node->airtime, AIRTIME_TIER_CRITICAL, rreq_airtime_ms);
                } else {
                    node->budget_denied[AIRTIME_IDX_CRITICAL]++;
                }
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
