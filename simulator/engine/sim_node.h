#ifndef SIM_NODE_H
#define SIM_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include "../../components/routing/include/channel_flood.h" /* FLOOD_RELAY_QUEUE_CAPACITY, FLOOD_SUPPRESS_AFTER */
#include "../../components/reliability/include/reliability.h"
#include "../../components/dedup/include/dedup.h"
#include "../../components/airtime/include/airtime_budget.h"
#include "../../components/fragment/include/fragment.h"
#include "../../components/crypto/include/crypto.h"
#include "../../components/security/include/security.h"
#include "../../main/beacon_policy_calc.h"
#include "sim_random.h"

/* Forward declaration only: the full definition lives in sim_radio.h, which
 * itself includes this header, so it cannot be included back here. node_tick
 * needs just a pointer, to gate beacon/RREQ transmissions through the real
 * airtime budget using the real radio ToA (radio_frame_airtime_ms). */
typedef struct radio_config radio_config_t;

#define MAX_NODES 256
#define NODE_ID_LEN 16

/* Tick intervals (microseconds). NODE_BEACON_INTERVAL_BASE_US is only used to
 * size the randomized first-beacon phase at boot (node_activate), so staggered
 * nodes do not all key up on the same tick; the actual beacon cadence is
 * decided every tick by beacon_interval_decide() (main/beacon_policy_calc.c),
 * driven by the scenario's sim_beacon_policy_t (see below). Per-beacon jitter
 * comes from the firmware's own beacon_next_interval_ms (span-clamped
 * BEACON_JITTER_MS, main/beacon_policy_calc.h). */
#define NODE_BEACON_INTERVAL_BASE_US 60000000ULL /* 60 s firmware base, boot-phase only */
#define NODE_NEIGHBOR_PURGE_US 60000000ULL       /* 60 s */
#define NODE_ROUTE_MAINT_US 60000000ULL          /* 60 s */
#define NODE_DISCOVERY_CHECK_US 5000000ULL       /*  5 s */
#define NODE_TICK_INTERVAL_US 1000000ULL         /*  1 s base tick */

/*
 * Beacon interval policy: scenario-wide configuration fed into the REAL
 * firmware decision function beacon_interval_decide() (main/beacon_policy_calc.c),
 * one shared instance for the whole sim, mirroring the single per-device
 * s_beacon_policy in mesh_task.c (every simulated node evaluates the same
 * policy against its own neighbor/churn state, exactly like every real
 * device runs the same shipped policy against its own local conditions).
 * Defaults mirror firmware's shipped config (mesh_task.c:298-306): adaptive
 * disabled (BEACON_MODE_FIXED), fixed 60 s. Overridable per scenario
 * (simulator/engine/sim_scenario.c "beacon" block).
 */
typedef struct {
    bool adaptive;            /* false = fixed cadence (firmware default) */
    uint32_t interval_ms;     /* base/fixed interval */
    uint32_t min_interval_ms; /* adaptive: high-churn floor */
    uint32_t max_interval_ms; /* adaptive: dense-mesh ceiling */
    uint8_t dense_threshold;  /* adaptive: neighbor_count >= this -> max_interval_ms */
    uint8_t churn_threshold;  /* adaptive: churn_events >= this -> min_interval_ms */
    uint32_t churn_window_ms; /* adaptive: churn lookback window */
} sim_beacon_policy_t;

/* Firmware's literal shipped defaults (mesh_task.c:298-306); not exported as
 * macros there, so mirrored here as the sim's firmware-default baseline. */
#define SIM_BEACON_POLICY_DEFAULT_INTERVAL_MS 60000u
#define SIM_BEACON_POLICY_DEFAULT_MIN_MS 30000u
#define SIM_BEACON_POLICY_DEFAULT_MAX_MS 120000u
#define SIM_BEACON_POLICY_DEFAULT_DENSE_THRESHOLD 10u
#define SIM_BEACON_POLICY_DEFAULT_CHURN_THRESHOLD 3u
#define SIM_BEACON_POLICY_DEFAULT_CHURN_WINDOW_MS 60000u

void sim_beacon_policy_init(sim_beacon_policy_t* policy);

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
    /* This node's slot in the owning node_array_t, assigned once at
     * node_array_add and stable for the node's life (a rejoin reuses the
     * entry). It is what keys the radio's link table in link mode, where
     * audibility is looked up per node pair rather than derived from x/y. */
    int index;
    float x;
    float y;
    /* Original scenario position, captured at creation: a coordinate-less
     * node_join restores the node here instead of leaving it wherever the
     * kill put it. */
    float home_x;
    float home_y;
    bool active;

    /* The node's PERSISTENT Ed25519 identity keypair, the sim analog of
     * firmware's NVS-stored identity. Created ONCE in node_array_add and
     * surviving leave/rejoin (node_activate models a reboot and must not
     * touch it), deterministic from the node id so scenario runs are
     * reproducible. addr above is DERIVED from ident_ed_pub exactly as
     * firmware derives it (crypto_derive_address = SHA256[0:4]), which
     * is what lets the REAL identity_store addr<->key check accept sim
     * attestations. */
    uint8_t ident_ed_pub[32];
    uint8_t ident_ed_priv[64];

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
    /* Channel flood: separate, src_addr-qualified dedup for the
     * broadcast/channel DATA flood path -- mirrors main/mesh_task.c's
     * s_flood_dedup. Kept apart from `dedup` above (which the RREQ path
     * uses keyed on raw packet_id) for the same collision-safety reason. */
    dedup_buffer_t flood_dedup;

    /* Rebroadcast suppression: per-node pending flood relays,
     * mirroring flood.go's floodSim.pending (keyed by flood_key) and
     * firmware's s_flood_relay_queue heard/cancel fields. A scheduled relay is
     * an EVT_SEND_PACKET already in the event queue with no cancel handle, so
     * cancellation is tracked HERE: an overheard duplicate bumps heard and, at
     * FLOOD_SUPPRESS_AFTER, sets canceled; bridge_handle_flood_relay checks the
     * flag when the relay comes due and skips the send. Kept the same
     * structure + threshold as the model so the two stay identical. */
    struct {
        bool used;
        uint32_t flood_key;
        uint8_t heard;
        bool canceled;
    } flood_pending[FLOOD_RELAY_QUEUE_CAPACITY];

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

    /* Beacon churn tracking (main/beacon_policy_calc.h churn_sample_t ring),
     * mirrors mesh_task's s_churn_history/s_churn_history_idx/
     * last_neighbor_count: per-node DYNAMIC state, reset on (re)activation.
     * The policy CONFIG itself (sim_beacon_policy_t) is scenario-wide, not
     * per-node; see sim_node.h's sim_beacon_policy_t. */
    churn_sample_t beacon_churn_history[MAX_CHURN_HISTORY];
    int beacon_churn_history_idx;
    uint8_t beacon_last_neighbor_count;
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
               const sim_beacon_policy_t* beacon_policy, node_tick_result_t* result);

#endif /* SIM_NODE_H */
