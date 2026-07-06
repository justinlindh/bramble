#include "bridge.h"
#include "../../components/packet/include/packet.h"
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include "../../components/routing/include/forwarding.h"
#include "../../components/routing/include/channel_flood.h"
#include "../../components/airtime/include/airtime_budget.h"
#include "../../components/fragment/include/fragment.h"
#include "../../components/crypto/include/crypto.h"
#include "../../components/routing_auth/include/routing_auth.h"
#include "../../components/network_key/include/network_key.h"
#include "../../components/identity/include/identity.h"
/* Note: mailbox.h, location.h,
 * channel_key.h, public_channel.h are all pulled in transitively via
 * bridge.h (Phase 6 headers). */

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ─── Extended per-node state (Phase 6) ─────────────────────────────────── */

static bridge_node_ext_t g_node_ext[MAX_NODES];
static bridge_ext_metrics_t g_ext_metrics;

/* ─── Intermediate-node RREP on/off switch (Phase 2 Part B) ─────────────── */
/* See bridge.h's doc comment: firmware always has this on; this exists so
 * gosim scenarios can A/B it. Defaults to true (the shipped behavior). */
static bool g_intermediate_rrep_enabled = true;

void bridge_set_intermediate_rrep_enabled(bool enabled) { g_intermediate_rrep_enabled = enabled; }

bool bridge_get_intermediate_rrep_enabled(void) { return g_intermediate_rrep_enabled; }

/* ─── Flood transport on/off switch (Flooding F1 Task 1) ────────────────── */
/* See bridge.h's doc comment: mirrors firmware's s_flood_transport. Default
 * false (reactive unchanged, matching firmware's shipped NVS default). */
static bool g_flood_transport_enabled = false;

void bridge_set_flood_transport_enabled(bool enabled) { g_flood_transport_enabled = enabled; }

bool bridge_get_flood_transport_enabled(void) { return g_flood_transport_enabled; }

/* ─── Flood-transport origination hop budget (Flooding F1 finalize) ──────── */
/* Mirrors firmware's s_flood_hop_limit: the operator-settable hop_limit a
 * freshly-originated flood DATA / flooded receipt is stamped with under
 * g_flood_transport_enabled (via flood_origination_hop_limit, the SAME helper
 * the firmware originators use). Default FLOOD_HOP_LIMIT_DEFAULT (8) so no
 * scenario that omits the field changes. Driven by the scenario's optional
 * "flood_hop_limit" field (sim.go). */
static uint8_t g_flood_hop_limit = FLOOD_HOP_LIMIT_DEFAULT;

void bridge_set_flood_hop_limit(uint8_t hops) { g_flood_hop_limit = flood_hop_limit_clamp(hops); }

uint8_t bridge_get_flood_hop_limit(void) { return g_flood_hop_limit; }

/* Public channel state (one global instance) */
static bramble_channel_t g_pub_channels[16];
static int g_num_pub_channels = 0;

bridge_node_ext_t* bridge_node_ext_get(int node_idx) {
    if (node_idx < 0 || node_idx >= MAX_NODES)
        return NULL;
    return &g_node_ext[node_idx];
}

bridge_ext_metrics_t* bridge_ext_metrics_get(void) { return &g_ext_metrics; }

/* Flooding F1 Task 2: _handle_delivery_receipt (defined above bridge_flood_
 * relay) floods the confirmation receipt back through the shared flood engine
 * under flood transport, so it needs this forward declaration. */
static bool bridge_flood_relay(sim_node_t* rx, const uint8_t* buf, uint16_t len,
                               const bramble_header_t* hdr, uint32_t orig_sender, bool is_own_echo,
                               uint64_t now_us, uint32_t now_ms, radio_config_t* radio,
                               pcg32_state_t* rng, event_queue_t* events);

void bridge_node_ext_init_all(void) {
    memset(&g_node_ext, 0, sizeof(g_node_ext));
    memset(&g_ext_metrics, 0, sizeof(g_ext_metrics));
    for (int i = 0; i < MAX_NODES; i++) {
        mailbox_init(&g_node_ext[i].mailbox);
        location_init(&g_node_ext[i].location);
        g_node_ext[i].initialized = true;
        /* Mandatory-provisioning (Task 2): every node is provisioned by default
         * (fleet shares bridge_init's default key); a scenario opts a node out. */
        g_node_ext[i].provisioned = true;
        /* Trust-anchor campaign (P2): every node is endorsed by default (the
         * fixed test anchor vouches for the whole fleet), so existing scenarios
         * still pin under the endorsed-only gate; a scenario opts a node out. */
        g_node_ext[i].endorsed = true;
    }
}

/*
 * Mandatory-provisioning (Task 2): shared default network key for the sim
 * fleet. The real network_key.c is a process-global (one address space for all
 * sim nodes), so provisioning it once at bridge_init makes every node's
 * control-plane MACs sign/verify consistently -- the sim analog of a fleet
 * whose nodes all hold the same provisioned key. Per-node inertness is modeled
 * by bridge_node_ext_t.provisioned, not by clearing this global. Value is
 * arbitrary but fixed and distinct from the host suites' key.
 */
static const uint8_t BRIDGE_DEFAULT_NET_KEY[32] = {
    0x51, 0x4d, 0x2a, 0x9e, 0x0c, 0xb7, 0x63, 0xf8, 0x11, 0xa4, 0xd0, 0x3c, 0x77, 0x8b, 0x2e, 0x59,
    0x40, 0x96, 0xe1, 0x1f, 0xcd, 0x84, 0x6a, 0x22, 0xb3, 0x5e, 0x08, 0xf7, 0x9c, 0x30, 0xab, 0x67};

/*
 * Trust-anchor campaign (P2): the sim fleet's fixed test anchor. bridge_init
 * expands this seed (RFC 8032) into the anchor keypair; the PUBLIC key is set
 * on every node's pin store (so nodes pin endorsed-only), and the PRIVATE key
 * signs each endorsed node's cert host-side (the offline anchor holder's job;
 * the device never signs endorsements). Fixed + distinct from the KAT anchor
 * so runs are deterministic.
 */
static const uint8_t BRIDGE_TEST_ANCHOR_SEED[32] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
static uint8_t g_bridge_anchor_pub[BRAMBLE_ED25519_PUBKEY_SIZE];
static uint8_t g_bridge_anchor_priv[BRAMBLE_ED25519_SECKEY_SIZE];

void bridge_node_set_provisioned(int node_idx, bool provisioned) {
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (ext)
        ext->provisioned = provisioned;
}

void bridge_node_set_endorsed(int node_idx, bool endorsed) {
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (ext)
        ext->endorsed = endorsed;
}

/* Trust-anchor campaign (P2 red-team): force a node's pin store un-anchored
 * (the "unanchored": true boot flag), undoing the join-time anchoring. The sim
 * analog of a node deployed BEFORE the operator provisioned a fleet anchor: it
 * pins on self-sig alone (TOFU) until a later provision_anchor event hardens
 * it. Touches the store struct directly (bridge is test scaffolding); no pins
 * exist yet at boot, so this only clears has_anchor. */
void bridge_node_set_anchored(int node_idx, bool anchored) {
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (!ext)
        return;
    if (anchored) {
        identity_store_set_anchor(&ext->ident_pins, g_bridge_anchor_pub);
    } else {
        ext->ident_pins.has_anchor = false;
    }
}

/* ─── Location sim helper: map (x,y) grid coords → pseudo lat/lon ─────── */
/* Treat grid origin as (37.0000000 N, -122.0000000 W), scale 1 unit = 10m */
#define LOC_REF_LAT_E7 370000000
#define LOC_REF_LON_E7 -1220000000
#define LOC_SCALE_E7 90 /* ~10m per grid unit in e7 degrees */

static void node_ext_set_sim_position(bridge_node_ext_t* ext, float x, float y) {
    bramble_position_t pos;
    memset(&pos, 0, sizeof(pos));
    pos.latitude_e7 = LOC_REF_LAT_E7 + (int32_t)(y * LOC_SCALE_E7);
    pos.longitude_e7 = LOC_REF_LON_E7 + (int32_t)(x * LOC_SCALE_E7);
    pos.altitude_m = 10;
    pos.accuracy_m = 5;
    pos.speed_kmh = 0;
    pos.heading_deg2 = 0;
    pos.timestamp = 0;
    pos.valid = true;
    location_set_position(&ext->location, &pos);
    g_ext_metrics.location_updates++;
}

/* ─── Relay path tracker (Phase 1) ─────────────────────────────────────── */
#define MAX_RELAY_PATHS 256
#define MAX_RELAY_HOPS 16

typedef struct {
    uint32_t packet_id;
    uint32_t hops[MAX_RELAY_HOPS];
    uint8_t hop_count;
    bool active;
} relay_path_entry_t;

static relay_path_entry_t g_relay_paths[MAX_RELAY_PATHS];

static void relay_path_init(void) { memset(g_relay_paths, 0, sizeof(g_relay_paths)); }

static void relay_path_add_hop(uint32_t packet_id, uint32_t relay_addr) {
    /* Find existing entry */
    for (int i = 0; i < MAX_RELAY_PATHS; i++) {
        if (g_relay_paths[i].active && g_relay_paths[i].packet_id == packet_id) {
            if (g_relay_paths[i].hop_count < MAX_RELAY_HOPS) {
                g_relay_paths[i].hops[g_relay_paths[i].hop_count++] = relay_addr;
            }
            return;
        }
    }
    /* Create new entry */
    for (int i = 0; i < MAX_RELAY_PATHS; i++) {
        if (!g_relay_paths[i].active) {
            g_relay_paths[i].active = true;
            g_relay_paths[i].packet_id = packet_id;
            g_relay_paths[i].hop_count = 1;
            g_relay_paths[i].hops[0] = relay_addr;
            return;
        }
    }
}

static relay_path_entry_t* relay_path_get(uint32_t packet_id) {
    for (int i = 0; i < MAX_RELAY_PATHS; i++) {
        if (g_relay_paths[i].active && g_relay_paths[i].packet_id == packet_id) {
            return &g_relay_paths[i];
        }
    }
    return NULL;
}

static void relay_path_remove(uint32_t packet_id) {
    for (int i = 0; i < MAX_RELAY_PATHS; i++) {
        if (g_relay_paths[i].active && g_relay_paths[i].packet_id == packet_id) {
            g_relay_paths[i].active = false;
            return;
        }
    }
}

/* ─── Crypto helpers (Phase 5) ─────────────────────────────────────────── */

/* Derive a shared symmetric key for a node pair from their addresses */
static void derive_pair_key(uint32_t addr_a, uint32_t addr_b, uint8_t* key_out) {
    uint32_t lo = (addr_a < addr_b) ? addr_a : addr_b;
    uint32_t hi = (addr_a < addr_b) ? addr_b : addr_a;
    uint8_t material[8 + 11]; /* two addresses + "bramble-sim" */
    material[0] = (lo >> 24) & 0xFF;
    material[1] = (lo >> 16) & 0xFF;
    material[2] = (lo >> 8) & 0xFF;
    material[3] = lo & 0xFF;
    material[4] = (hi >> 24) & 0xFF;
    material[5] = (hi >> 16) & 0xFF;
    material[6] = (hi >> 8) & 0xFF;
    material[7] = hi & 0xFF;
    memcpy(material + 8, "bramble-sim", 11);
    crypto_sha256(material, sizeof(material), key_out);
}

/* ─── Global simulation time ───────────────────────────────────────────── */
uint64_t g_bridge_sim_time_us = 0;

void bridge_set_sim_time(uint64_t us) { g_bridge_sim_time_us = us; }

uint32_t sim_get_time_ms(void) { return (uint32_t)(g_bridge_sim_time_us / 1000); }

/* ─── Control-plane airtime budget gate ────────────────────────────────── */

/* AIRTIME_TIER_* -> AIRTIME_IDX_* (mirrors the private tier_idx in
 * components/airtime/airtime_budget.c, which is not exported: the sim only
 * needs it to pick a budget_denied[] slot, never to change the actual
 * accounting, which always goes through the real airtime_budget_can_transmit
 * / airtime_budget_debit). */
static int airtime_tier_idx(uint8_t tier) {
    switch (tier) {
    case AIRTIME_TIER_CRITICAL:
        return AIRTIME_IDX_CRITICAL;
    case AIRTIME_TIER_BROADCAST:
        return AIRTIME_IDX_BROADCAST;
    case AIRTIME_TIER_RECEIPT:
        return AIRTIME_IDX_RECEIPT;
    default:
        return AIRTIME_IDX_NORMAL;
    }
}

/* Single chokepoint for every control-plane transmission (RREQ/RREP/RERR
 * origination and forwarding, delivery receipts): mirrors tx_gate_transmit's
 * check -> transmit -> debit sequence using the node's real airtime budget,
 * with the peer-count scaler refreshed from the current neighbor table first
 * (matching mesh_tx's tx_gate_set_peer_count call on every transmit). On
 * denial the packet is dropped, no queue, matching tx_gate's drop-no-queue
 * semantics, and the tier's budget_denied counter is incremented. Returns
 * true iff the packet was actually put on the air, so callers only count a
 * forward/send in their own stats when it really happened. */
static bool budget_gated_send(sim_node_t* tx, const outbound_packet_t* pkt, uint8_t tier,
                              node_array_t* nodes, radio_config_t* radio, pcg32_state_t* rng,
                              event_queue_t* events, metrics_state_t* metrics, uint64_t send_us) {
    uint32_t airtime_ms = radio_frame_airtime_ms(radio, pkt->len);
    airtime_budget_set_mesh_size(&tx->airtime, (uint8_t)neighbor_count(&tx->neighbors));
    if (!airtime_budget_can_transmit(&tx->airtime, tier, airtime_ms)) {
        tx->budget_denied[airtime_tier_idx(tier)]++;
        return false;
    }
    airtime_budget_debit(&tx->airtime, tier, airtime_ms);
    sim_radio_broadcast(tx, pkt, nodes, radio, rng, events, metrics, send_us);
    return true;
}

/* ─── Event union accessors ────────────────────────────────────────────── */

node_event_data_t bridge_get_node_event(const sim_event_t* e) { return e->data.node; }

packet_event_data_t bridge_get_packet_event(const sim_event_t* e) { return e->data.packet; }

tick_event_data_t bridge_get_tick_event(const sim_event_t* e) { return e->data.tick; }

interference_event_data_t bridge_get_interference_event(const sim_event_t* e) {
    return e->data.interference;
}

event_type_t bridge_get_event_type(const sim_event_t* e) { return e->type; }

uint64_t bridge_get_event_timestamp(const sim_event_t* e) { return e->timestamp_us; }

/* ─── Event construction helpers ───────────────────────────────────────── */

sim_event_t bridge_make_tick_event(uint64_t ts_us, const char* node_id, uint32_t tick_seq) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_TICK_NODE;
    e.timestamp_us = ts_us;
    strncpy(e.data.tick.node_id, node_id, NODE_ID_LEN - 1);
    e.data.tick.tick_seq = tick_seq;
    return e;
}

sim_event_t bridge_make_node_event(event_type_t type, uint64_t ts_us, const char* node_id,
                                   uint32_t addr, float x, float y) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.timestamp_us = ts_us;
    strncpy(e.data.node.node_id, node_id, NODE_ID_LEN - 1);
    e.data.node.addr = addr;
    e.data.node.x = x;
    e.data.node.y = y;
    return e;
}

sim_event_t bridge_make_generate_msg_event(uint64_t ts_us, const char* node_id,
                                           uint32_t dest_addr) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_GENERATE_MESSAGE;
    e.timestamp_us = ts_us;
    strncpy(e.data.node.node_id, node_id, NODE_ID_LEN - 1);
    e.data.node.addr = dest_addr;
    return e;
}

sim_event_t bridge_make_interference_start(uint64_t ts_us, float cx, float cy, float radius) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_INTERFERENCE_START;
    e.timestamp_us = ts_us;
    e.data.interference.center_x = cx;
    e.data.interference.center_y = cy;
    e.data.interference.radius = radius;
    return e;
}

sim_event_t bridge_make_interference_end(uint64_t ts_us, int zone_index) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_INTERFERENCE_END;
    e.timestamp_us = ts_us;
    e.data.interference.zone_index = zone_index;
    return e;
}

sim_event_t bridge_make_receive_packet_event(uint64_t ts_us, uint32_t src_addr, uint32_t dest_addr,
                                             const uint8_t* data, uint16_t len) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_RECEIVE_PACKET;
    e.timestamp_us = ts_us;
    e.data.packet.src_addr = src_addr;
    e.data.packet.dest_addr = dest_addr;
    e.data.packet.air_start_us = ts_us;
    e.data.packet.air_end_us = ts_us;
    if (len > sizeof(e.data.packet.data))
        len = (uint16_t)sizeof(e.data.packet.data);
    memcpy(e.data.packet.data, data, len);
    e.data.packet.len = len;
    return e;
}

/*
 * bridge_make_flood_relay_event (Phase 2 Task 0, managed-flooding routing
 * mode): builds a due-timestamped EVT_SEND_PACKET carrying a scheduled
 * rebroadcast for gosim's Go-only flood.go, exactly the same event-union
 * construction problem bridge_make_flood_relay_event's Task 5 sibling
 * (_handle_data's broadcast branch) solves inline in C: cgo cannot set
 * fields inside a C union from Go, so any code building a sim_event_t
 * whose payload is the packet union needs a tiny C constructor. Unlike
 * Task 5's relay (which is fired by bridge_handle_flood_relay, the AODV
 * DATA-broadcast path), this event is only ever produced and consumed by
 * flood.go's own EVT_SEND_PACKET handler (dispatchEvent branches on
 * routing mode), so node_addr/frame are flood.go's own wire format, not
 * bramble_header_t. data.packet.src_addr carries the RELAYING node's own
 * address (which node this rebroadcast is due on), matching the Task 5
 * convention.
 */
sim_event_t bridge_make_flood_relay_event(uint64_t due_us, uint32_t node_addr, const uint8_t* frame,
                                          uint16_t len) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_SEND_PACKET;
    e.timestamp_us = due_us;
    e.data.packet.src_addr = node_addr;
    if (len > sizeof(e.data.packet.data))
        len = (uint16_t)sizeof(e.data.packet.data);
    memcpy(e.data.packet.data, frame, len);
    e.data.packet.len = len;
    return e;
}

/* ─── Message tracking ─────────────────────────────────────────────────── */

void bridge_msg_track_init(msg_tracker_t* track, int count) {
    memset(track, 0, sizeof(msg_tracker_t) * count);
}

int bridge_msg_track_add(msg_tracker_t* track, int count, uint32_t packet_id, uint32_t src_addr,
                         uint32_t dest_addr, uint64_t sent_us) {
    for (int i = 0; i < count; i++) {
        if (!track[i].active) {
            track[i].active = true;
            track[i].packet_id = packet_id;
            track[i].src_addr = src_addr;
            track[i].dest_addr = dest_addr;
            track[i].sent_us = sent_us;
            track[i].attempt = 1;
            track[i].confirmed = false;
            return i;
        }
    }
    return -1;
}

uint32_t bridge_msg_track_find_src(msg_tracker_t* track, int count, uint32_t packet_id) {
    for (int i = 0; i < count; i++) {
        if (track[i].active && track[i].packet_id == packet_id) {
            return track[i].src_addr;
        }
    }
    return 0;
}

bool bridge_msg_track_complete(msg_tracker_t* track, int count, uint32_t packet_id, uint64_t now_us,
                               metrics_state_t* metrics) {
    for (int i = 0; i < count; i++) {
        if (track[i].active && track[i].packet_id == packet_id) {
            uint64_t latency_us = now_us - track[i].sent_us;
            metrics_record_packet_delivered(metrics, latency_us);
            track[i].active = false;
            return true;
        }
    }
    return false;
}

bool bridge_msg_track_confirm(msg_tracker_t* track, int count, uint32_t packet_id,
                              metrics_state_t* metrics) {
    for (int i = 0; i < count; i++) {
        if (track[i].packet_id == packet_id && !track[i].confirmed) {
            /* A freshly-memset (never-used) slot also has packet_id == 0
             * and confirmed == false; guard against matching it when the
             * real packet_id happens to be 0 by requiring the slot to have
             * been used at least once (src_addr set at track_add time is
             * never 0 for a real node address). */
            if (track[i].src_addr == 0)
                continue;
            track[i].confirmed = true;
            metrics_record_packet_confirmed(metrics);
            return true;
        }
    }
    return false;
}

/* ─── Internal packet handlers ─────────────────────────────────────────── */

static void _handle_beacon(sim_node_t* rx, const uint8_t* buf, uint16_t len, int8_t rssi,
                           uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                           node_anomaly_tracker_t* anomaly) {
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, buf, len) != ESP_OK)
        return;

    neighbor_update(&rx->neighbors, beacon.src_addr, rssi, 0, beacon.pubkey_hash, now_ms);

    /* Phase 1 Task 1 (delivery-core plan): firmware's handle_beacon
     * (main/mesh_task.c) never installs a route on a heard beacon, only the
     * neighbor_update above. The sim used to install a direct route to every
     * beacon sender here, which accidentally supplied the reverse-hop route
     * that relays never get in real firmware, masking the confirmation-return
     * bug: relays only ever learn routes TOWARD a discovery target via RREP
     * (see _handle_rrep / rrep_rx_decide), never back toward a message's
     * originator. Route installation now happens exclusively via RREQ/RREP,
     * matching firmware, so the sim can reproduce that bug instead of hiding
     * it. anomaly is now unused here since the beacon-triggered route-flap
     * check went away with the route_install it guarded. */
    (void)anomaly;
    int node_idx = (int)(rx - nodes->nodes);
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);

    /* Phase 6: Mailbox — check if we have stored messages for the beacon sender */
    if (ext && ext->mailbox.count > 0) {
        mailbox_entry_t pending[MAILBOX_MAX_PER_DEST];
        int n = mailbox_retrieve(&ext->mailbox, beacon.src_addr, pending, MAILBOX_MAX_PER_DEST);
        if (n > 0) {
            g_ext_metrics.mailbox_delivered += (uint64_t)n;
            fprintf(stdout,
                    "{\"type\":\"mailbox_delivered\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"count\":%d}\n",
                    (unsigned long long)now_us, rx->id, beacon.src_addr, n);
            fflush(stdout);
        }
    }
}

static void _handle_rreq(sim_node_t* rx, const uint8_t* buf, uint16_t len, int8_t rssi,
                         uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                         radio_config_t* radio, pcg32_state_t* rng, event_queue_t* events,
                         metrics_state_t* metrics, node_anomaly_tracker_t* anomaly) {
    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, buf, len) != ESP_OK)
        return;

    if (rreq_dedup_check_and_add(&rx->rreq_dedup, rreq.query_id, now_ms))
        return;

    reverse_route_add(&rx->reverse_routes, rreq.query_id, rreq.prev_hop, now_ms);

    if (rreq.header.dest_addr == rx->addr) {
        bramble_rrep_t rrep = rrep_build_destination(&rreq, rx->addr);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rrep_serialize(&rrep, pkt.data, RREP_SIZE);
        pkt.len = RREP_SIZE;
        pkt.is_broadcast = false;
        pkt.dest_addr = rreq.prev_hop;
        pkt.pkt_type = PKT_TYPE_RREP;

        /* tx_gate_kind_tier: TX_KIND_ROUTING -> AIRTIME_TIER_CRITICAL. */
        budget_gated_send(rx, &pkt, AIRTIME_TIER_CRITICAL, nodes, radio, rng, events, metrics,
                          now_us);
    } else if (g_intermediate_rrep_enabled &&
               intermediate_rrep_route_usable(route_lookup(&rx->routes, rreq.header.dest_addr),
                                              now_ms)) {
        /* Phase 2 "save reactive routing" Part B: intermediate-node RREP.
         * Mirrors main/mesh_task.c's handle_rreq using the SAME real
         * component functions (intermediate_rrep_route_usable,
         * rrep_build_intermediate) so the sim cannot drift from firmware's
         * trust/freshness rules. g_intermediate_rrep_enabled (default true)
         * exists only so a gosim scenario can A/B this feature on identical
         * topology/traffic (see bridge.h); firmware has no such switch, it
         * is always on.
         *
         * Unlike firmware's handle_rreq, this does not redraw/re-sign a
         * fresh seq: gosim's destination-reply branch just above never has
         * either (rrep_build_destination's own internal rrep_sign, with
         * seq left at its zeroed default, is what ships here), since gosim
         * does not model the ws 1.3b replay window at all. Matching that
         * existing simplification keeps this branch internally consistent
         * with gosim's own destination path rather than firmware's fuller
         * one. Same reasoning for skipping the reverse route_install to the
         * RREQ source that firmware's destination AND intermediate
         * branches both do: gosim's destination branch here does not do it
         * either. */
        route_entry_t* cached_route = route_lookup(&rx->routes, rreq.header.dest_addr);
        bramble_rrep_t rrep = rrep_build_intermediate(&rreq, cached_route, rx->addr, rssi, 0);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rrep_serialize(&rrep, pkt.data, RREP_SIZE);
        pkt.len = RREP_SIZE;
        pkt.is_broadcast = false;
        pkt.dest_addr = rreq.prev_hop;
        pkt.pkt_type = PKT_TYPE_RREP;

        /* tx_gate_kind_tier: TX_KIND_ROUTING -> AIRTIME_TIER_CRITICAL. Having
         * replied, this node does NOT also forward the RREQ (see
         * main/mesh_task.c's handle_rreq for the airtime-saving/safety
         * rationale): no fall-through to the forward branch below. */
        budget_gated_send(rx, &pkt, AIRTIME_TIER_CRITICAL, nodes, radio, rng, events, metrics,
                          now_us);
    } else if (rreq.header.hop_limit > 1) {
        /* Global forwarded-RREQ token bucket (SEC-M4), same decision point as
         * firmware's handle_rreq (main/mesh_task.c:2460): gated AFTER the
         * dedup check above, BEFORE building the forward packet or scheduling
         * the jittered rebroadcast. Denied means dropped, same as firmware
         * (which only logs and returns; no queue). */
        if (!rreq_fwd_allow(&rx->rreq_fwd, now_ms)) {
            rx->rreq_fwd_denied++;
            return;
        }

        bramble_rreq_t fwd = rreq_forward(&rreq, rx->addr, rssi, 0);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rreq_serialize(&fwd, pkt.data, RREQ_SIZE);
        pkt.len = RREQ_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr = 0xFFFFFFFF;
        pkt.pkt_type = PKT_TYPE_RREQ;

        {
            int node_idx = (int)(rx - nodes->nodes);
            anomaly_check_rreq_retx(&anomaly[node_idx].rreq_retx, rreq.header.dest_addr, now_us,
                                    stdout, rx->id);
        }

        /* Jittered rebroadcast (DES-3), same 50-300ms window as firmware,
         * so same-hop relays do not key up at the same instant. */
        uint64_t jitter_us = (uint64_t)discovery_forward_jitter_ms(pcg32_random(rng)) * 1000ULL;
        if (budget_gated_send(rx, &pkt, AIRTIME_TIER_CRITICAL, nodes, radio, rng, events, metrics,
                              now_us + jitter_us)) {
            rx->packets_forwarded++;
        }
    }
}

static void _handle_rrep(sim_node_t* rx, const uint8_t* buf, uint16_t len, uint32_t pkt_src_addr,
                         uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                         radio_config_t* radio, pcg32_state_t* rng, event_queue_t* events,
                         metrics_state_t* metrics, node_anomaly_tracker_t* anomaly) {
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, buf, len) != ESP_OK)
        return;

    /* Use the real firmware decision (rrep_rx_decide) rather than a hand-rolled
     * copy, so the simulator cannot drift from mesh_task.c's handle_rrep: the
     * next_hop = forwarder-address fix and the discovery-participation install
     * gate both come for free. The simulator does not apply a link penalty, so
     * it passes rrep.route_metric through as the link metric. pkt_src_addr is
     * now unused: rrep.next_hop already carries the forwarder's own address. */
    (void)pkt_src_addr;
    int node_idx = (int)(rx - nodes->nodes);
    rrep_rx_decision_t d = rrep_rx_decide(&rrep, rx->addr, rrep.route_metric,
                                          &rx->pending_discoveries, &rx->reverse_routes);

    if (d.install_route) {
        route_install(&rx->routes, d.route_dest, d.route_next_hop, d.route_hops, d.route_metric,
                      ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, now_ms);
        emit_route_added(stdout, now_us, rx->id, d.route_dest, d.route_next_hop, d.route_hops);
        anomaly_check_route_flap(&anomaly[node_idx].flap, d.route_dest, d.route_next_hop, now_us,
                                 stdout, rx->id);
    }

    switch (d.action) {
    case RREP_RX_DELIVER:
        discovery_remove(&rx->pending_discoveries, d.deliver_dest);
        return;
    case RREP_RX_FORWARD: {
        bramble_rrep_t fwd = rrep_forward(&rrep, d.forward_to, rx->addr);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rrep_serialize(&fwd, pkt.data, RREP_SIZE);
        pkt.len = RREP_SIZE;
        pkt.is_broadcast = false;
        pkt.dest_addr = d.forward_to;
        pkt.pkt_type = PKT_TYPE_RREP;

        /* tx_gate_kind_tier: TX_KIND_ROUTING -> AIRTIME_TIER_CRITICAL. */
        if (budget_gated_send(rx, &pkt, AIRTIME_TIER_CRITICAL, nodes, radio, rng, events, metrics,
                              now_us)) {
            rx->packets_forwarded++;
        }
        return;
    }
    case RREP_RX_DROP:
        return;
    }
}

static void _handle_rerr(sim_node_t* rx, const uint8_t* buf, uint16_t len, uint64_t now_us,
                         uint32_t now_ms) {
    (void)now_ms;
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, buf, len) != ESP_OK)
        return;

    rerr_handle(&rx->routes, &rerr);
    emit_link_broken(stdout, now_us, rx->id, rerr.broken_next_hop);
}

static void _handle_delivery_receipt(sim_node_t* rx, const uint8_t* buf, uint16_t len,
                                     uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                                     radio_config_t* radio, pcg32_state_t* rng,
                                     event_queue_t* events, metrics_state_t* metrics,
                                     node_anomaly_tracker_t* anomaly, msg_tracker_t* msg_track,
                                     int msg_track_count) {
    bramble_delivery_receipt_t receipt;
    if (bramble_delivery_receipt_deserialize(&receipt, buf, len) != ESP_OK)
        return;

    if (receipt.header.dest_addr == rx->addr) {
        /* This receipt is for us — the original sender */
        bridge_msg_track_complete(msg_track, msg_track_count, receipt.orig_packet_id, now_us,
                                  metrics);

        /* Phase 2 "save reactive routing" Part A: this is the TRUE
         * confirmed-delivery signal (confirmed_packets, feeding
         * confirmed_delivery_rate), distinct from the destination-reach
         * signal (delivered_packets, feeding message_delivery_rate) the
         * bridge_msg_track_complete call above records at DATA arrival, one
         * call up the stack in _handle_data. That earlier call already
         * deactivated this packet_id's tracker entry, so the
         * bridge_msg_track_complete call right above (same packet_id) is a
         * no-op by design; this is genuinely the only place a receipt
         * reaching the ORIGINATOR can be observed. bridge_msg_track_confirm
         * looks the entry up by packet_id regardless of `active` and
         * de-dupes on its own `confirmed` flag, so a second receipt for the
         * same message (e.g. both of two DATA retries succeeding) is not
         * double-counted. */
        bridge_msg_track_confirm(msg_track, msg_track_count, receipt.orig_packet_id, metrics);

        /* Check if this was a retried message */
        pending_ack_t* pa = NULL;
        for (int i = 0; i < MAX_PENDING_ACKS; i++) {
            if (rx->pending_acks.entries[i].active &&
                rx->pending_acks.entries[i].packet_id == receipt.orig_packet_id) {
                pa = &rx->pending_acks.entries[i];
                break;
            }
        }
        if (pa && pa->attempt > 1) {
            metrics->messages_delivered_retry++;
        }
        /* Always clear the pending ack on receipt */
        pending_ack_remove(&rx->pending_acks, receipt.orig_packet_id);

        /* Emit delivered with path */
        fprintf(stdout,
                "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"packet_id\":\"0x%08X\""
                ",\"from\":\"0x%08X\",\"hops\":%d,\"path\":[",
                (unsigned long long)now_us, rx->id, receipt.orig_packet_id, receipt.src_addr,
                receipt.hop_count);
        for (int i = 0; i < receipt.hop_count && i < DELIVERY_RECEIPT_MAX_HOPS; i++) {
            if (i > 0)
                fprintf(stdout, ",");
            fprintf(stdout, "\"0x%08X\"", receipt.relay_path[i]);
        }
        fprintf(stdout, "]}\n");
        fflush(stdout);
        return;
    }

    /* Flooding F1 Task 2: under flood transport there are no routes, so the
     * confirmation (gosim's delivery receipt, the packet that feeds
     * confirmed_delivery_rate exactly as the firmware ACK does) FLOODS back to
     * the originator through the SAME engine DATA floods with (bridge_flood_
     * relay: src-qualified dedup + channel_flood_decide + jittered relay +
     * FLOOD_SUPPRESS_AFTER). receipt.src_addr is the destination that
     * originated the receipt; a node hearing its OWN receipt echoed back
     * (is_own_echo) never rebroadcasts it. The re-injected relay dispatches on
     * the wire header type (PKT_TYPE_DELIVERY_RECEIPT), so it re-enters this
     * handler at the next hop with no route table consulted anywhere. When
     * flood transport is off, the reactive route-lookup forward below is
     * unchanged. */
    if (g_flood_transport_enabled) {
        bool is_own_echo = (receipt.src_addr == rx->addr);
        bridge_flood_relay(rx, buf, len, &receipt.header, receipt.src_addr, is_own_echo, now_us,
                           now_ms, radio, rng, events);
        return;
    }

    /* Forward the receipt toward its destination */
    uint8_t hop_limit = receipt.header.hop_limit;
    forward_result_t fwd_res =
        forward_data(&rx->routes, receipt.header.dest_addr, &hop_limit, now_ms);
    if (!fwd_res.should_send)
        return;

    uint8_t fwd_buf[256];
    memcpy(fwd_buf, buf, len);
    fwd_buf[3] = hop_limit; /* update hop_limit */

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, fwd_buf, len);
    pkt.len = len;
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DELIVERY_RECEIPT;

    /* tx_gate_kind_tier: TX_KIND_RECEIPT -> AIRTIME_TIER_RECEIPT. */
    if (budget_gated_send(rx, &pkt, AIRTIME_TIER_RECEIPT, nodes, radio, rng, events, metrics,
                          now_us)) {
        rx->packets_forwarded++;
    }
}

/* Flooding F1 rebroadcast suppression (gosim-bridge parity with firmware's
 * channel_flood_note_overheard + s_flood_relay_queue and flood.go's
 * floodSim.pending): record a node's scheduled relay so a later overheard
 * duplicate can find and cancel it. */
static void bridge_flood_pending_add(sim_node_t* rx, uint32_t flood_key) {
    for (int i = 0; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        if (!rx->flood_pending[i].used) {
            rx->flood_pending[i].used = true;
            rx->flood_pending[i].flood_key = flood_key;
            rx->flood_pending[i].heard = 0;
            rx->flood_pending[i].canceled = false;
            return;
        }
    }
    /* Queue full: leave the relay uncancellable (it still fires), matching
     * firmware's fall-back-to-immediate-relay-when-full posture. */
}

/* Register one overheard duplicate of flood_key against rx's pending relays;
 * cancel the matching entry once FLOOD_SUPPRESS_AFTER copies are in. The key
 * is src-qualified (packet_id ^ orig_sender), so a duplicate from a different
 * originator never matches. Mirrors channel_flood_note_overheard /
 * floodSim.noteHeardDuplicate exactly. */
static void bridge_flood_pending_note_overheard(sim_node_t* rx, uint32_t flood_key) {
    for (int i = 0; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        if (!rx->flood_pending[i].used || rx->flood_pending[i].flood_key != flood_key ||
            rx->flood_pending[i].canceled) {
            continue;
        }
        rx->flood_pending[i].heard++;
        if (rx->flood_pending[i].heard >= FLOOD_SUPPRESS_AFTER) {
            rx->flood_pending[i].canceled = true;
        }
        return;
    }
}

/* When rx's relay for flood_key comes due, report whether it was canceled by
 * overheard copies and release the slot. Returns true iff the caller must
 * SKIP the send (relay was suppressed). A miss (no matching slot, e.g. queue
 * was full at schedule time) returns false -> relay fires, matching the
 * uncancellable fallback in bridge_flood_pending_add. */
static bool bridge_flood_pending_take_canceled(sim_node_t* rx, uint32_t flood_key) {
    for (int i = 0; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        if (!rx->flood_pending[i].used || rx->flood_pending[i].flood_key != flood_key) {
            continue;
        }
        bool canceled = rx->flood_pending[i].canceled;
        rx->flood_pending[i].used = false;
        return canceled;
    }
    return false;
}

/* Flooding F1 Task 1: the ONE flood relay engine shared by broadcast DATA
 * (always flood-eligible) and, when g_flood_transport_enabled, unicast DATA
 * not addressed to the receiving node. Mirrors main/mesh_task.c's handle_data
 * exactly (same dedup key, same channel_flood_decide call, same jittered
 * relay scheduling): no parallel flood implementation in gosim either.
 * is_own_echo is a caller-supplied input (rather than recomputed here)
 * because the broadcast caller also needs it to gate its own per-hearer
 * "delivered" notification. Returns the dedup result (is_dup) for the same
 * reason. */
static bool bridge_flood_relay(sim_node_t* rx, const uint8_t* buf, uint16_t len,
                               const bramble_header_t* hdr, uint32_t orig_sender, bool is_own_echo,
                               uint64_t now_us, uint32_t now_ms, radio_config_t* radio,
                               pcg32_state_t* rng, event_queue_t* events) {
    /* Src_addr-qualified dedup key (rx->flood_dedup): see the broadcast
     * caller's original comment (and firmware's s_flood_dedup) for why a
     * plain packet_id key risks a cross-source collision on the flood
     * path. */
    uint32_t flood_key = hdr->packet_id ^ orig_sender;
    bool is_dup = dedup_check_and_add(&rx->flood_dedup, flood_key, now_ms);

    /* Flooding F1 suppression: unlike firmware, bridge's dispatch-gate dedup
     * (rx->dedup) is RREQ-only, so a duplicate DATA reaches here every time
     * with is_dup==true (firmware instead counts these at its s_dedup gate,
     * see mesh_task.c). This IS the overheard-copy the pending relay counts:
     * bump heard on the matching src-qualified entry and cancel it once
     * FLOOD_SUPPRESS_AFTER copies are in. The FIRST copy is not a duplicate
     * (is_dup false -> schedules below), so it is never counted here.
     *
     * Whole-branch review note: firmware gates its two overheard-copy counters
     * on the copy's network-key MAC verifying first (data_auth_verify /
     * ack_verify in mesh_task.c), so a keyless party cannot forge a duplicate
     * to cancel a genuine relay. No equivalent gate is needed here because
     * gosim models only honest, key-holding nodes and never injects forged /
     * bad-MAC frames: every duplicate that reaches this path is a genuine
     * re-flood by another honest relay (data_rx_decide's auth gate is assumed
     * already passed, see the DATA_RX_DELIVER comment below). The firmware MAC
     * gate is the load-bearing fix; this stays a faithful honest-node model. */
    if (is_dup) {
        bridge_flood_pending_note_overheard(rx, flood_key);
    }

    /* Non-mutating pre-check against the real BROADCAST-lane airtime budget
     * (tx_gate_kind_tier: TX_KIND_DATA_BROADCAST -> AIRTIME_TIER_BROADCAST),
     * same tier the jittered relay send below debits from for real. */
    airtime_budget_set_mesh_size(&rx->airtime, (uint8_t)neighbor_count(&rx->neighbors));
    uint32_t relay_airtime_ms = radio_frame_airtime_ms(radio, len);
    bool budget_permits =
        airtime_budget_can_transmit(&rx->airtime, AIRTIME_TIER_BROADCAST, relay_airtime_ms);

    channel_flood_decision_t flood = channel_flood_decide(hdr->hop_limit, is_dup || is_own_echo,
                                                          budget_permits, pcg32_random(rng));

    if (flood.should_relay) {
        uint8_t relay_buf[256];
        memcpy(relay_buf, buf, len);
        bramble_header_t relay_hdr = *hdr;
        relay_hdr.hop_limit = flood.new_hop_limit;
        bramble_header_serialize(&relay_hdr, relay_buf, HEADER_SIZE);

        /* Schedule the jittered relay by pushing a due-timestamped event,
         * the sim's natural equivalent of main/mesh_task.c's polled
         * due_at_ms queue (see the original broadcast-only comment this was
         * lifted from). */
        sim_event_t relay_evt;
        memset(&relay_evt, 0, sizeof(relay_evt));
        relay_evt.timestamp_us = now_us + (uint64_t)flood.jitter_ms * 1000ULL;
        relay_evt.type = EVT_SEND_PACKET;
        relay_evt.data.packet.src_addr = rx->addr;
        /* Flooding F1: carry the src-qualified flood_key on the relay event's
         * otherwise-unused dest_addr field (bridge_handle_flood_relay always
         * rewrites dest to 0xFFFFFFFF on TX), so the due handler can look up
         * this node's pending entry and honor a cancellation. gosim DATA has
         * no wire-embedded src_addr to recompute the key from at due time (the
         * originator is tracked out-of-band by packet_id), so it must ride the
         * event. */
        relay_evt.data.packet.dest_addr = flood_key;
        relay_evt.data.packet.len = len;
        memcpy(relay_evt.data.packet.data, relay_buf, len);
        event_queue_push(events, &relay_evt);

        /* Track the scheduled relay so an overheard duplicate can cancel it
         * (mirrors firmware's schedule_flood_relay recording flood_key/heard
         * and flood.go's floodScheduleRelay populating floodSim.pending). */
        bridge_flood_pending_add(rx, flood_key);
    }
    return is_dup;
}

static void _handle_data(sim_node_t* rx, const uint8_t* buf, uint16_t len, uint32_t pkt_src_addr,
                         int8_t rssi, int8_t snr, uint64_t now_us, uint32_t now_ms,
                         node_array_t* nodes, radio_config_t* radio, pcg32_state_t* rng,
                         event_queue_t* events, metrics_state_t* metrics,
                         node_anomaly_tracker_t* anomaly, msg_tracker_t* msg_track,
                         int msg_track_count) {
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK)
        return;

    int node_idx = (int)(rx - nodes->nodes);

    anomaly_record_rx(&anomaly[node_idx].blackhole, now_us);
    anomaly_check_loop(&anomaly[node_idx].loop, hdr.packet_id, now_us, stdout, rx->id);

    /* Gosim has no wire-embedded src_addr/prev_hop for DATA (its framing
     * already diverges from firmware's, see task-4-report.md): the DATA's
     * ORIGINATOR is tracked out-of-band by packet_id
     * (bridge_msg_track_add, set once at origination), and prev_hop is the
     * radio layer's own notion of "who transmitted this specific frame"
     * (pkt_src_addr == event->data.packet.src_addr, set by sim_radio.c on
     * every single broadcast/retransmit; a relay's retransmit is a new
     * broadcast from the relay, so this is already exactly prev_hop
     * semantics with zero extra plumbing needed on the TX side). */
    uint32_t orig_sender = bridge_msg_track_find_src(msg_track, msg_track_count, hdr.packet_id);

    /* Same data_rx_decide the firmware calls (main/mesh_task.c,
     * mesh_process_rx_packet's PKT_TYPE_DATA case): no parallel
     * deliver/forward-vs-reverse-route logic here. orig_sender == 0 means
     * this packet_id was never tracked (should not happen for in-flight
     * traffic; guarded defensively so a lookup miss cannot install a
     * dest=0 route, a gosim-only concern since firmware always has a real
     * wire src_addr and never sees "unknown"). */
    uint8_t data_link_metric = metric_apply_link_penalty(255, rssi, snr);
    data_rx_decision_t data_rx = data_rx_decide(hdr.dest_addr, rx->addr, orig_sender, pkt_src_addr,
                                                hdr.hop_limit, data_link_metric);
    if (data_rx.install_reverse_route && orig_sender != 0) {
        route_install(&rx->routes, data_rx.reverse_dest, data_rx.reverse_next_hop,
                      data_rx.reverse_hop_count, data_rx.reverse_metric, ROUTE_ACTIVE,
                      ROUTE_SRC_BREADCRUMB, now_ms);
        emit_route_added(stdout, now_us, rx->id, data_rx.reverse_dest, data_rx.reverse_next_hop,
                         data_rx.reverse_hop_count);
    }

    /* Task 5 (channel flood): a broadcast dest (0xFFFFFFFF) also decides
     * DATA_RX_DELIVER above, but it is NOT the unicast case the block below
     * handles -- there is no single destination, no pair-key to decrypt
     * under, and no private delivery receipt to send home. Split here,
     * BEFORE the unicast-only logic, exactly like main/mesh_task.c's
     * handle_data places its flood-relay decision before the decrypt fork:
     * relaying does not require decrypting (data_rx_decide's auth gate --
     * modeled in firmware by data_auth_verify -- already establishes this
     * frame is genuine before we ever get here), so a node without this
     * channel's key still floods the exact bytes onward for members
     * further out in the mesh. */
    if (data_rx.action == DATA_RX_DELIVER && hdr.dest_addr == 0xFFFFFFFF) {
        /* A mesh flood means a node hears its own originated broadcast
         * echoed back once some neighbor rebroadcasts it; folded into
         * is_duplicate for channel_flood_decide (same "already seen,
         * nothing to gain by relaying again" rule), mirroring firmware's
         * identical is_own_echo guard in main/mesh_task.c's handle_data.
         * Also gates the message_delivered emit just below: the originator
         * hearing its own flood echoed back is not a new delivery anywhere,
         * it is the same node that already originated (and locally
         * delivered) this message, so it must not be counted or reported as
         * a fresh delivery (mirrors main/mesh_task.c's handle_data
         * src_addr == s_identity->address self-guard). */
        bool is_own_echo = (orig_sender != 0 && orig_sender == rx->addr);

        /* bridge_flood_relay (above) owns the dedup check + channel_flood_
         * decide + jittered relay scheduling, shared verbatim with the Task
         * 1 unicast-flood branch below (no parallel flood implementation);
         * it returns is_dup so the per-hearer "delivered" notification below
         * (broadcast-only: a flood has no single destination) can gate on
         * the SAME dedup result instead of re-checking. */
        bool is_dup = bridge_flood_relay(rx, buf, len, &hdr, orig_sender, is_own_echo, now_us,
                                         now_ms, radio, rng, events);

        if (!is_dup && !is_own_echo) {
            /* Every hearer "delivers" locally (a broadcast has no single
             * destination); emit a message_delivered signal per hearer so a
             * scenario can prove reach at a far node, whether or not that
             * node holds the channel key gosim does not model.
             *
             * Deliberately does NOT call bridge_msg_track_complete (unlike
             * the unicast path below): that call deactivates the shared
             * msg_track entry globally on its first invocation from ANY
             * node, and orig_sender resolution above depends on that same
             * entry staying active for the rest of the flood's lifetime
             * (every hop still needs to resolve the true originator for its
             * OWN dedup key). Completing it early would silently start
             * returning orig_sender == 0 to every later hop, corrupting the
             * dedup key mid-flood. Consequence: message_delivery_rate (the
             * legacy scenarios' headline metric, unicast-oriented) does not
             * count broadcast deliveries; reach is observed instead via
             * these message_delivered events, matching how the gosim test
             * for Task 5 asserts >=3-hop delivery. */
            fprintf(stdout,
                    "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"packet_id\":\"0x%08X\"}\n",
                    (unsigned long long)now_us, rx->id, hdr.packet_id);
            fflush(stdout);
        }
        return;
    }

    /* Flooding F1 Task 1: g_flood_transport_enabled turns a unicast frame
     * NOT addressed to this node into a relay-only pass-through through the
     * SAME flood engine as the broadcast branch above (bridge_flood_relay),
     * instead of falling into the forward_data route lookup further below.
     * This node is never the destination here (data_rx.action ==
     * DATA_RX_FORWARD means dest_addr is neither self nor broadcast), so
     * there is nothing to decrypt or deliver locally, no delivery receipt to
     * send, and no message_delivered notification to emit; it only
     * propagates the frame onward, mirroring main/mesh_task.c's handle_data
     * returning right after its relay decision for a frame that is not
     * addressed to self. When g_flood_transport_enabled is false, this
     * branch does not trigger and behavior is exactly the pre-Task-1
     * reactive route lookup below. */
    if (data_rx.action == DATA_RX_FORWARD && g_flood_transport_enabled) {
        bool is_own_echo = (orig_sender != 0 && orig_sender == rx->addr);
        bridge_flood_relay(rx, buf, len, &hdr, orig_sender, is_own_echo, now_us, now_ms, radio, rng,
                           events);
        return;
    }

    if (data_rx.action == DATA_RX_DELIVER) {
        /* Message reached destination: send delivery receipt back to source */

        /* Crypto: decrypt payload if present (Phase 5) */
        uint8_t decrypted_payload[256];
        uint16_t payload_len = (len > HEADER_SIZE) ? (len - HEADER_SIZE) : 0;
        bool crypto_ok = true;
        if (payload_len > (BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE) && orig_sender != 0) {
            const uint8_t* enc_data = buf + HEADER_SIZE;
            const uint8_t* nonce = enc_data;
            const uint8_t* tag = enc_data + BRAMBLE_NONCE_SIZE;
            const uint8_t* ct = enc_data + BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE;
            size_t ct_len = payload_len - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;

            uint8_t key[BRAMBLE_KEY_SIZE];
            derive_pair_key(orig_sender, rx->addr, key);
            /* AAD binds the header (hop_limit masked, as firmware does since
             * the PR #79 AAD binding); must match the originator's AAD. */
            uint8_t aad[HEADER_SIZE];
            bramble_header_build_aad(&hdr, aad, sizeof(aad));
            if (crypto_aes256gcm_decrypt(key, nonce, ct, ct_len, aad, HEADER_SIZE, tag,
                                         decrypted_payload) == 0) {
                metrics->crypto_decrypted++;
            } else {
                metrics->crypto_auth_failed++;
                crypto_ok = false;
            }
        }

        if (!crypto_ok) {
            emit_packet_dropped(stdout, now_us, rx->id, "crypto_auth_fail");
            return;
        }

        /* Fragment reassembly (Phase 4): check if payload has frag header */
        if (payload_len > (BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE + FRAG_HEADER_SIZE)) {
            /* Decrypted payload may contain fragment */
            size_t pt_len = payload_len - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;
            frag_header_t fhdr;
            fhdr.frag_index = decrypted_payload[0];
            fhdr.frag_total = decrypted_payload[1];
            fhdr.message_id = (uint16_t)(decrypted_payload[2] | (decrypted_payload[3] << 8));

            if (fhdr.frag_total > 1 && fhdr.frag_total <= FRAG_MAX_FRAGMENTS) {
                /* This is a fragment */
                int result =
                    reassembly_add(&rx->reassembly, &fhdr, decrypted_payload + FRAG_HEADER_SIZE,
                                   pt_len - FRAG_HEADER_SIZE, now_ms, hdr.packet_id);
                if (result < 1) {
                    /* Not yet complete or error */
                    return;
                }
                /* Complete! Collect reassembled message */
                uint8_t reassembled[1024];
                int rlen = reassembly_collect(&rx->reassembly, fhdr.message_id, reassembled,
                                              sizeof(reassembled));
                if (rlen > 0) {
                    metrics->fragments_reassembled++;
                }
                /* Fall through to delivery */
            }
        } else if (payload_len > FRAG_HEADER_SIZE &&
                   payload_len <= HEADER_SIZE + FRAG_HEADER_SIZE + FRAG_MAX_PLAINTEXT &&
                   !(payload_len > (BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE))) {
            /* Unencrypted fragment check */
            const uint8_t* pl = buf + HEADER_SIZE;
            frag_header_t fhdr;
            fhdr.frag_index = pl[0];
            fhdr.frag_total = pl[1];
            fhdr.message_id = (uint16_t)(pl[2] | (pl[3] << 8));

            if (fhdr.frag_total > 1 && fhdr.frag_total <= FRAG_MAX_FRAGMENTS) {
                int result = reassembly_add(&rx->reassembly, &fhdr, pl + FRAG_HEADER_SIZE,
                                            payload_len - FRAG_HEADER_SIZE, now_ms, hdr.packet_id);
                if (result < 1)
                    return;
                uint8_t reassembled[1024];
                int rlen = reassembly_collect(&rx->reassembly, fhdr.message_id, reassembled,
                                              sizeof(reassembled));
                if (rlen > 0)
                    metrics->fragments_reassembled++;
            }
        }

        /* Record delivery immediately (don't wait for receipt to arrive at source) */
        bridge_msg_track_complete(msg_track, msg_track_count, hdr.packet_id, now_us, metrics);

        anomaly_record_fwd(&anomaly[node_idx].blackhole, now_us);

        if (orig_sender != 0) {
            /* Build and send delivery receipt with relay path */
            relay_path_entry_t* rp = relay_path_get(hdr.packet_id);

            bramble_delivery_receipt_t receipt;
            memset(&receipt, 0, sizeof(receipt));
            receipt.header.version = BRAMBLE_VERSION;
            receipt.header.type = PKT_TYPE_DELIVERY_RECEIPT;
            receipt.header.flags = 0;
            /* Reactive: ROUTE_HOP_LIMIT_MAX. Flood transport: the flooded
             * receipt originates at the operator-settable flood hop budget,
             * mirroring firmware's send_ack. */
            receipt.header.hop_limit =
                flood_origination_hop_limit(g_flood_transport_enabled, g_flood_hop_limit);
            receipt.header.dest_addr = orig_sender;
            receipt.header.packet_id = pcg32_random(rng);
            receipt.src_addr = rx->addr;
            receipt.orig_packet_id = hdr.packet_id;

            /* Populate relay path from tracker */
            if (rp) {
                receipt.hop_count = rp->hop_count;
                for (int i = 0; i < rp->hop_count && i < DELIVERY_RECEIPT_MAX_HOPS; i++) {
                    receipt.relay_path[i] = rp->hops[i];
                }
                relay_path_remove(hdr.packet_id);
            } else {
                receipt.hop_count = 0;
            }

            uint8_t receipt_buf[DELIVERY_RECEIPT_MAX_SIZE];
            uint16_t receipt_len = DELIVERY_RECEIPT_MIN_SIZE + receipt.hop_count * 4;
            if (bramble_delivery_receipt_serialize(&receipt, receipt_buf, sizeof(receipt_buf)) ==
                ESP_OK) {
                if (g_flood_transport_enabled) {
                    /* Flooding F1 Task 2: originate the confirmation as a
                     * flood, not a routed unicast. There are no routes back to
                     * the sender under flood transport, so the destination
                     * broadcasts the receipt once (header.dest_addr stays the
                     * originator, so only it consumes) and every neighbor
                     * flood-relays it via _handle_delivery_receipt. This is the
                     * exact analogue of firmware send_ack's single mesh_tx that
                     * neighbors then flood. */
                    outbound_packet_t pkt;
                    memset(&pkt, 0, sizeof(pkt));
                    memcpy(pkt.data, receipt_buf, receipt_len);
                    pkt.len = receipt_len;
                    pkt.is_broadcast = true;
                    pkt.dest_addr = 0xFFFFFFFF;
                    pkt.pkt_type = PKT_TYPE_DELIVERY_RECEIPT;
                    budget_gated_send(rx, &pkt, AIRTIME_TIER_BROADCAST, nodes, radio, rng, events,
                                      metrics, now_us);
                } else {
                    /* Route receipt back to sender */
                    uint8_t rcpt_hop = 32;
                    forward_result_t fwd_res =
                        forward_data(&rx->routes, orig_sender, &rcpt_hop, now_ms);
                    if (fwd_res.should_send) {
                        outbound_packet_t pkt;
                        memset(&pkt, 0, sizeof(pkt));
                        memcpy(pkt.data, receipt_buf, receipt_len);
                        pkt.len = receipt_len;
                        pkt.is_broadcast = false;
                        pkt.dest_addr = fwd_res.next_hop;
                        pkt.pkt_type = PKT_TYPE_DELIVERY_RECEIPT;

                        /* tx_gate_kind_tier: TX_KIND_RECEIPT -> AIRTIME_TIER_RECEIPT. */
                        budget_gated_send(rx, &pkt, AIRTIME_TIER_RECEIPT, nodes, radio, rng, events,
                                          metrics, now_us);
                    }
                }
            }
        }

        /* Emit delivered event (receipt will update with path info when it arrives at source) */
        fprintf(stdout,
                "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"packet_id\":\"0x%08X\"}\n",
                (unsigned long long)now_us, rx->id, hdr.packet_id);
        fflush(stdout);
        return;
    }

    uint8_t hop_limit = hdr.hop_limit;
    forward_result_t fwd_res = forward_data(&rx->routes, hdr.dest_addr, &hop_limit, now_ms);

    if (fwd_res.route_error) {
        bramble_rerr_t rerr = rerr_build(rx->addr, hdr.dest_addr, fwd_res.next_hop);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rerr_serialize(&rerr, pkt.data, RERR_SIZE);
        pkt.len = RERR_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr = 0xFFFFFFFF;
        pkt.pkt_type = PKT_TYPE_RERR;

        /* tx_gate_kind_tier: TX_KIND_ROUTING -> AIRTIME_TIER_CRITICAL. */
        budget_gated_send(rx, &pkt, AIRTIME_TIER_CRITICAL, nodes, radio, rng, events, metrics,
                          now_us);
        emit_packet_dropped(stdout, now_us, rx->id, "no_route");

        /* Phase 6: Mailbox — store the DATA payload for the offline destination.
         * This relay node volunteers to hold the message until the dest rejoins. */
        bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
        if (ext && len > HEADER_SIZE) {
            /* Store the raw packet payload (everything after the header).
             * orig_sender is the tracker lookup hoisted to the top of this
             * function (Task 4); this branch used to repeat it. */
            if (orig_sender != 0) {
                int stored =
                    mailbox_store(&ext->mailbox, orig_sender, hdr.dest_addr, buf + HEADER_SIZE,
                                  len - HEADER_SIZE, hdr.packet_id, now_ms);
                if (stored == 0) {
                    g_ext_metrics.mailbox_stored++;
                    fprintf(stdout,
                            "{\"type\":\"mailbox_stored\",\"timestamp_us\":%llu"
                            ",\"node\":\"%s\",\"dest\":\"0x%08X\""
                            ",\"packet_id\":\"0x%08X\",\"queued\":%d}\n",
                            (unsigned long long)now_us, rx->id, hdr.dest_addr, hdr.packet_id,
                            ext->mailbox.count);
                    fflush(stdout);
                }
            }
        }
        return;
    }

    if (!fwd_res.should_send)
        return;

    /* Track relay path (Phase 1) */
    relay_path_add_hop(hdr.packet_id, rx->addr);

    uint8_t fwd_buf[256];
    memcpy(fwd_buf, buf, len);
    fwd_buf[3] = hop_limit;

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, fwd_buf, len);
    pkt.len = len;
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;

    anomaly_record_fwd(&anomaly[node_idx].blackhole, now_us);

    /* tx_gate_kind_tier: TX_KIND_FORWARD -> AIRTIME_TIER_NORMAL. This site had
     * no budget check at all before Task 1 (unlike the retransmit/originate
     * DATA sites), a gap the "every simulated transmission" audit missed. */
    if (budget_gated_send(rx, &pkt, AIRTIME_TIER_NORMAL, nodes, radio, rng, events, metrics,
                          now_us)) {
        rx->packets_forwarded++;
    }
}

/*
 * bridge_handle_flood_relay: fires when a jittered channel-flood relay
 * (scheduled by the broadcast branch of _handle_data above) comes due.
 * event->data.packet.src_addr is the RELAYING node's own address (not a
 * radio source -- see the scheduling comment in _handle_data);
 * event->data.packet.data/len is the exact relay-mutated frame (hop_limit
 * already decremented, header already re-serialized) to transmit.
 *
 * The real airtime budget gets the final say here, exactly like
 * main/mesh_task.c's process_flood_relay_queue -> mesh_tx: a node that had
 * budget when it decided to relay but has since spent it (its own traffic,
 * or other jittered relays firing first) still yields instead of
 * transmitting -- the airtime-aware stop that keeps a saturated node from
 * amplifying a storm.
 */
void bridge_handle_flood_relay(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                               pcg32_state_t* rng, event_queue_t* events,
                               metrics_state_t* metrics) {
    sim_node_t* tx = node_array_find_by_addr(nodes, event->data.packet.src_addr);
    if (!tx || !tx->active)
        return;

    /* Flooding F1 rebroadcast suppression: the scheduling side stashed this
     * relay's src-qualified flood_key on the event's dest_addr field. If
     * enough OTHER copies were overheard while it waited out its jitter
     * (FLOOD_SUPPRESS_AFTER), skip the now-redundant send -- exactly what
     * firmware's process_flood_relay_queue does by finding used==false, and
     * flood.go's handleFloodRelayDue does via p.canceled. Emit a
     * flood_relay_suppressed signal so scenarios can prove the cancellation
     * fired and count it against the model's flood_relays_canceled. */
    if (bridge_flood_pending_take_canceled(tx, event->data.packet.dest_addr)) {
        fprintf(stdout,
                "{\"type\":\"flood_relay_suppressed\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\"}\n",
                (unsigned long long)event->timestamp_us, tx->id);
        fflush(stdout);
        return;
    }

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, event->data.packet.data, event->data.packet.len);
    pkt.len = event->data.packet.len;
    pkt.is_broadcast = true;
    pkt.dest_addr = 0xFFFFFFFF;
    /* Flooding F1 Task 2: the flood engine now carries both DATA and the
     * confirmation receipt, so derive the emitter/airtime pkt_type from the
     * wire header rather than assuming DATA. Dispatch at the receiver is on
     * the wire type regardless; this only keeps TX metrics/logs accurate. */
    bramble_header_t relay_hdr;
    if (bramble_header_deserialize(&relay_hdr, event->data.packet.data, event->data.packet.len) ==
        ESP_OK) {
        pkt.pkt_type = relay_hdr.type;
    } else {
        pkt.pkt_type = PKT_TYPE_DATA;
    }

    /* tx_gate_kind_tier: TX_KIND_DATA_BROADCAST -> AIRTIME_TIER_BROADCAST. */
    if (budget_gated_send(tx, &pkt, AIRTIME_TIER_BROADCAST, nodes, radio, rng, events, metrics,
                          event->timestamp_us)) {
        tx->packets_forwarded++;
    }
}

/* ─── Identity attestation RX (per-node identity Phase 3) ──────────────── */
/*
 * Mirrors main/mesh_task.c's handle_identity_attestation with the same
 * verification ORDER: exact-length deserialize, then the CHEAP network-key
 * relay-gate MAC (ident_relay_verify) before anything else (fail = drop:
 * no relay, no pinning, no Ed25519 verify), then deliver to the node's
 * TOFU pin store REGARDLESS of the relay decision, then flood-relay
 * through the shared engine (bridge_flood_relay: same dedup key, same
 * channel_flood_decide, same jittered schedule; frame unmodified except
 * hop_limit). Relays never Ed25519-verify; only the pin store does.
 *
 * Divergence from firmware, both precedented in this file: no
 * control-replay window (gosim does not model ws 1.3b replay at all, see
 * _handle_rreq's intermediate-RREP note), and suppression counting happens
 * inside bridge_flood_relay's is_dup branch rather than at a dispatch
 * dedup gate (gosim's dispatch dedup is RREQ-only, see bridge_flood_relay).
 */
static void _handle_identity_attestation(sim_node_t* rx, const uint8_t* buf, uint16_t len,
                                         uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                                         radio_config_t* radio, pcg32_state_t* rng,
                                         event_queue_t* events) {
    bramble_identity_attestation_t att;
    if (bramble_identity_attestation_deserialize(&att, buf, len) != ESP_OK)
        return;

    /* Relay gate: the cheap MAC, FIRST. A keyless frame dies here at its
     * first hop: never pinned, never relayed, never Ed25519-verified. */
    if (!ident_relay_verify(&att))
        return;

    int node_idx = (int)(rx - nodes->nodes);
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (ext) {
        /* Deliver locally regardless of the relay decision below: the one
         * receive-side Ed25519 verify + TOFU pin (identity_store.h). epoch_ms=0:
         * gosim does not model a synced wall clock, so cert expiry is not
         * enforced (v1 certs are permanent anyway; see identity_store.h). */
        identity_pin_result_t pin =
            identity_store_handle_attestation(&ext->ident_pins, &att, rx->addr, now_ms, 0);
        if (pin == IDENTITY_PIN_NEW) {
            fprintf(stdout,
                    "{\"type\":\"identity_pinned\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"addr\":\"0x%08X\",\"ed8\":\"%02X%02X%02X%02X\"}\n",
                    (unsigned long long)now_us, rx->id, att.src_addr, att.ed25519_pub[0],
                    att.ed25519_pub[1], att.ed25519_pub[2], att.ed25519_pub[3]);
            fflush(stdout);
        } else if (pin == IDENTITY_PIN_CONFLICT) {
            /* Impersonation detected: first-seen wins, the original
             * binding survives; report both sides so scenarios can assert
             * WHICH keys were kept. */
            const identity_pin_t* kept = identity_store_lookup(&ext->ident_pins, att.src_addr);
            fprintf(stdout,
                    "{\"type\":\"identity_conflict\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"addr\":\"0x%08X\""
                    ",\"kept_ed8\":\"%02X%02X%02X%02X\""
                    ",\"rejected_ed8\":\"%02X%02X%02X%02X\"}\n",
                    (unsigned long long)now_us, rx->id, att.src_addr, kept->ed25519_pub[0],
                    kept->ed25519_pub[1], kept->ed25519_pub[2], kept->ed25519_pub[3],
                    att.ed25519_pub[0], att.ed25519_pub[1], att.ed25519_pub[2], att.ed25519_pub[3]);
            fflush(stdout);
        } else if (pin == IDENTITY_PIN_ADDR_MISMATCH) {
            /* Phase 4 addr<->key binding: the claimed address is not the
             * one the frame's own Ed25519 key derives to. Refused on
             * first contact, no pin required. */
            fprintf(stdout,
                    "{\"type\":\"identity_addr_mismatch\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"addr\":\"0x%08X\",\"ed8\":\"%02X%02X%02X%02X\"}\n",
                    (unsigned long long)now_us, rx->id, att.src_addr, att.ed25519_pub[0],
                    att.ed25519_pub[1], att.ed25519_pub[2], att.ed25519_pub[3]);
            fflush(stdout);
        } else if (pin == IDENTITY_PIN_UNENDORSED) {
            /* Trust-anchor gate (P2): this receiver is anchored and the
             * attestation carried no valid fleet-anchor cert. NOT pinned; the
             * frame is still relayed below (endorsement gates pinning only). */
            fprintf(stdout,
                    "{\"type\":\"identity_unendorsed\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"addr\":\"0x%08X\",\"ed8\":\"%02X%02X%02X%02X\"}\n",
                    (unsigned long long)now_us, rx->id, att.src_addr, att.ed25519_pub[0],
                    att.ed25519_pub[1], att.ed25519_pub[2], att.ed25519_pub[3]);
            fflush(stdout);
        }
    }

    /* Flood relay through the ONE shared engine (no second flood
     * implementation): orig_sender is the frame's own MAC-covered src_addr
     * (unlike DATA, which tracks it out-of-band by packet_id). */
    bool is_own_echo = (att.src_addr == rx->addr);
    bridge_flood_relay(rx, buf, len, &att.header, att.src_addr, is_own_echo, now_us, now_ms, radio,
                       rng, events);
}

/* ─── Public packet handling wrappers ──────────────────────────────────── */

void bridge_handle_receive_packet(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                                  pcg32_state_t* rng, event_queue_t* events,
                                  metrics_state_t* metrics, node_anomaly_tracker_t* anomaly,
                                  msg_tracker_t* msg_track, int msg_track_count) {
    sim_node_t* rx = node_array_find_by_addr(nodes, event->data.packet.dest_addr);
    if (!rx || !rx->active)
        return;

    /* Mandatory-provisioning (Task 2): an unprovisioned receiver is INERT. It
     * holds no network key, so every network-key-authenticated frame fails its
     * verify (beacon/DATA-origin/RREP/RERR/ACK/receipt/attestation). Drop at
     * the door so it accepts nothing, mirroring firmware's per-verify rejects
     * plus the handle_beacon gate. */
    {
        bridge_node_ext_t* rext = bridge_node_ext_get((int)(rx - nodes->nodes));
        if (rext && !rext->provisioned)
            return;
    }

    uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);
    const uint8_t* buf = event->data.packet.data;
    uint16_t len = event->data.packet.len;
    int8_t rssi = event->data.packet.rssi;

    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK)
        return;

    /* Collision model: evaluate the reception at delivery time, when every
     * transmission that could overlap this packet's air window is in the
     * channel log. */
    radio_rx_outcome_t rx_outcome = radio_check_reception(radio, rx, &event->data.packet);
    if (rx_outcome == RADIO_RX_COLLISION || rx_outcome == RADIO_RX_HALF_DUPLEX) {
        if (rx_outcome == RADIO_RX_COLLISION) {
            metrics->collisions++;
        } else {
            metrics->half_duplex_drops++;
        }
        /* Emit drops for unicast traffic only; broadcast collisions are
         * metric-only (mirrors the radio_loss drop policy in sim_radio). */
        if (hdr.type == PKT_TYPE_DATA || hdr.type == PKT_TYPE_RREP ||
            hdr.type == PKT_TYPE_DELIVERY_RECEIPT) {
            emit_packet_dropped(stdout, event->timestamp_us, rx->id,
                                rx_outcome == RADIO_RX_COLLISION ? "collision" : "half_duplex");
        }
        return;
    }
    if (rx_outcome == RADIO_RX_CAPTURED)
        metrics->capture_wins++;
    metrics->receptions_ok++;

    /* Dedup check — only for broadcast packets (RREQ flood prevention).
     * Unicast packets (DATA, RREP, DELIVERY_RECEIPT) are forwarded hop-by-hop
     * with the same packet_id, so dedup would incorrectly drop them at relays. */
    if (hdr.type == PKT_TYPE_RREQ) {
        /* RREQs have their own rreq_dedup in _handle_rreq, but we also
         * check the general dedup to catch other broadcast floods */
        if (dedup_check_and_add(&rx->dedup, hdr.packet_id, now_ms)) {
            metrics->dedup_dropped++;
            return; /* duplicate */
        }
    }

    emit_packet_received_typed(stdout, event->timestamp_us, rx->id, event->data.packet.src_addr,
                               rssi, event->data.packet.snr, len, hdr.type);
    rx->packets_received++;

    switch (hdr.type) {
    case PKT_TYPE_BEACON:
        _handle_beacon(rx, buf, len, rssi, event->timestamp_us, now_ms, nodes, anomaly);
        break;
    case PKT_TYPE_RREQ:
        _handle_rreq(rx, buf, len, rssi, event->timestamp_us, now_ms, nodes, radio, rng, events,
                     metrics, anomaly);
        break;
    case PKT_TYPE_RREP:
        _handle_rrep(rx, buf, len, event->data.packet.src_addr, event->timestamp_us, now_ms, nodes,
                     radio, rng, events, metrics, anomaly);
        break;
    case PKT_TYPE_RERR:
        _handle_rerr(rx, buf, len, event->timestamp_us, now_ms);
        break;
    case PKT_TYPE_DATA:
        _handle_data(rx, buf, len, event->data.packet.src_addr, rssi, event->data.packet.snr,
                     event->timestamp_us, now_ms, nodes, radio, rng, events, metrics, anomaly,
                     msg_track, msg_track_count);
        break;
    case PKT_TYPE_DELIVERY_RECEIPT:
        _handle_delivery_receipt(rx, buf, len, event->timestamp_us, now_ms, nodes, radio, rng,
                                 events, metrics, anomaly, msg_track, msg_track_count);
        break;
    case PKT_TYPE_IDENTITY_ATTESTATION:
        _handle_identity_attestation(rx, buf, len, event->timestamp_us, now_ms, nodes, radio, rng,
                                     events);
        break;
    default:
        break;
    }
}

/* Sim-local nonce builder (src_addr || counter || 4 random bytes). The
 * firmware generates fully random nonces per message in channel_msg.c; the
 * crypto RFC replaces both schemes with a persisted deterministic counter
 * nonce, at which point this helper goes away too. */
static void sim_build_nonce(uint32_t src_addr, uint32_t counter, uint8_t* nonce) {
    nonce[0] = (uint8_t)(src_addr >> 24);
    nonce[1] = (uint8_t)(src_addr >> 16);
    nonce[2] = (uint8_t)(src_addr >> 8);
    nonce[3] = (uint8_t)src_addr;
    nonce[4] = (uint8_t)(counter >> 24);
    nonce[5] = (uint8_t)(counter >> 16);
    nonce[6] = (uint8_t)(counter >> 8);
    nonce[7] = (uint8_t)counter;
    crypto_random(nonce + 8, 4);
}

void bridge_handle_generate_message(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                                    pcg32_state_t* rng, event_queue_t* events,
                                    metrics_state_t* metrics, node_anomaly_tracker_t* anomaly,
                                    msg_tracker_t* msg_track, int msg_track_count) {
    sim_node_t* src = node_array_find_by_id(nodes, event->data.node.node_id);
    if (!src || !src->active)
        return;

    /* Mandatory-provisioning (Task 2): an unprovisioned node is INERT -- it
     * holds no network key, so it originates no authenticated DATA. */
    {
        bridge_node_ext_t* sext = bridge_node_ext_get((int)(src - nodes->nodes));
        if (sext && !sext->provisioned) {
            fprintf(stdout,
                    "{\"type\":\"unprovisioned_inert\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"frame\":\"data\"}\n",
                    (unsigned long long)event->timestamp_us, src->id);
            fflush(stdout);
            return;
        }
    }

    uint32_t dest_addr = event->data.node.addr;
    uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);

    /* Task 5 (channel flood): broadcast/channel message origination. There
     * is no destination route to discover (a broadcast has no single next
     * hop) and no retry ladder (mesh_send_broadcast/mesh_send_channel in
     * main/mesh_task.c are fire-and-forget too): one BROADCAST-lane
     * budget-gated transmission, tracked via msg_track so a relay's
     * src_addr-qualified flood dedup (bridge_handle_receive_packet's
     * _handle_data, below) can resolve the true originator, mirroring how
     * firmware carries src_addr directly on the DATA wire.
     *
     * The payload is sent in the clear (no derive_pair_key encryption):
     * that helper derives a key from a single (src, dest) pair, which does
     * not exist for a broadcast with N recipients, and gosim's DATA framing
     * already diverges from firmware's wire layout for this exact reason
     * (see the _handle_data comment on orig_sender tracking). Airtime is
     * still charged honestly off the real wire_len (header + payload). */
    if (dest_addr == 0xFFFFFFFF) {
        int payload_size = (int)event->data.node.x;
        if (payload_size <= 0)
            payload_size = 20; /* representative chat-sized payload */
        if (payload_size > 200)
            payload_size = 200;

        bramble_header_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.version = BRAMBLE_VERSION;
        hdr.type = PKT_TYPE_DATA;
        hdr.flags = 0;
        hdr.hop_limit = flood_origination_hop_limit(g_flood_transport_enabled, g_flood_hop_limit);
        hdr.dest_addr = 0xFFFFFFFF;
        hdr.packet_id = pcg32_random(rng);

        bridge_msg_track_add(msg_track, msg_track_count, hdr.packet_id, src->addr, 0xFFFFFFFF,
                             event->timestamp_us);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_header_serialize(&hdr, pkt.data, HEADER_SIZE);
        for (int i = 0; i < payload_size; i++) {
            pkt.data[HEADER_SIZE + i] = (uint8_t)(pcg32_random(rng) & 0xFF);
        }
        pkt.len = (uint16_t)(HEADER_SIZE + payload_size);
        pkt.is_broadcast = true;
        pkt.dest_addr = 0xFFFFFFFF;
        pkt.pkt_type = PKT_TYPE_DATA;
        src->packets_originated++;

        /* tx_gate_kind_tier: TX_KIND_DATA_BROADCAST -> AIRTIME_TIER_BROADCAST. */
        if (budget_gated_send(src, &pkt, AIRTIME_TIER_BROADCAST, nodes, radio, rng, events, metrics,
                              event->timestamp_us)) {
            metrics_record_message_sent(metrics);
            fprintf(stdout,
                    "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
                    ",\"node\":\"%s\",\"dest\":\"broadcast\",\"packet_id\":\"0x%08X\"}\n",
                    (unsigned long long)event->timestamp_us, src->id, hdr.packet_id);
        } else {
            metrics_record_packet_dropped(metrics);
            emit_packet_dropped(stdout, event->timestamp_us, src->id, "airtime_budget");
        }
        fflush(stdout);
        return;
    }

/* Retry limit: y field counts retry attempts (x is unused for generate_message).
 * After MAX_MSG_RETRIES attempts (~30s at 1.5s intervals), drop the message. */
#define MAX_MSG_RETRIES 20
    int retry_count = (int)event->data.node.y;

    if (retry_count >= MAX_MSG_RETRIES) {
        metrics_record_packet_dropped(metrics);
        fprintf(stdout,
                "{\"type\":\"message_dropped\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"reason\":\"retry_timeout\""
                ",\"retries\":%d}\n",
                (unsigned long long)event->timestamp_us, src->id, dest_addr, retry_count);
        fflush(stdout);
        return;
    }

    /* Flooding F1 Task 3: send-side flood origination. Under
     * g_flood_transport_enabled there is NO route discovery (mirrors firmware's
     * mesh_send_message gate): skip the reactive route_lookup + RREQ/retry
     * ladder entirely. The DATA is flooded immediately below (broadcast at
     * ROUTE_HOP_LIMIT_MAX with header.dest_addr = D), every relay floods it
     * (bridge_flood_relay), the destination floods a receipt back (Task 2), and
     * the pending_ack retry re-floods on no-confirmation
     * (bridge_handle_retransmit). When the toggle is off the reactive discovery
     * path is exactly as before. */
    route_entry_t* route = g_flood_transport_enabled ? NULL : route_lookup(&src->routes, dest_addr);

    if (!g_flood_transport_enabled &&
        (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING)) {
        /* Discovery cadence mirrors firmware (mesh_task discovery retries +
         * discovery_should_retry): first RREQ immediately, retries after 5 s
         * then 15 s, each retry under a FRESH query_id with an expanded hop
         * ring (DES-1/DES-2), 3 attempts total, then the discovery is
         * abandoned. The message-level retry below stands in for the
         * firmware's queued-message store and can start a fresh discovery
         * once the failed one is removed. */
        pending_discovery_t* pd = discovery_lookup(&src->pending_discoveries, dest_addr);
        bool should_send_rreq = false;
        if (!pd) {
            /* Fresh discovery origination: same decision point and gate as
             * firmware's initiate_discovery (main/mesh_task.c:4320-4322).
             * Discovery retries (the branch below and node_tick's discovery
             * retry check) are NOT rate-limited in firmware either: only the
             * first RREQ for a dest goes through rreq_rate_allow; retries are
             * throttled by their own discovery_should_retry backoff cadence. */
            if (!rreq_rate_allow(&src->rreq_rate, src->addr, dest_addr, now_ms)) {
                src->rreq_rate_denied++;
            } else {
                uint32_t query_id = pcg32_random(rng);
                discovery_start(&src->pending_discoveries, dest_addr, query_id, now_ms);
                should_send_rreq = true;
            }
        } else if (discovery_should_retry(pd, now_ms)) {
            discovery_record_attempt(pd, pcg32_random(rng), now_ms);
            should_send_rreq = true;
        } else if (pd->attempts >= MAX_RREQ_ATTEMPTS &&
                   (now_ms - pd->timestamp) > RREQ_RETRY_INTERVAL_2_MS) {
            /* Exhausted: abandon so a later send can start a fresh discovery */
            discovery_remove(&src->pending_discoveries, dest_addr);
        }

        if (should_send_rreq) {
            pd = discovery_lookup(&src->pending_discoveries, dest_addr);
            bramble_rreq_t rreq =
                rreq_build_originator(src->addr, dest_addr, discovery_current_query_id(pd),
                                      src->addr, discovery_hop_limit_for_attempt(pd->attempts));

            outbound_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            bramble_rreq_serialize(&rreq, pkt.data, RREQ_SIZE);
            pkt.len = RREQ_SIZE;
            pkt.is_broadcast = true;
            pkt.dest_addr = 0xFFFFFFFF;
            pkt.pkt_type = PKT_TYPE_RREQ;

            /* tx_gate_kind_tier: TX_KIND_ROUTING -> AIRTIME_TIER_CRITICAL. */
            budget_gated_send(src, &pkt, AIRTIME_TIER_CRITICAL, nodes, radio, rng, events, metrics,
                              event->timestamp_us);

            {
                int src_idx = (int)(src - nodes->nodes);
                anomaly_check_rreq_retx(&anomaly[src_idx].rreq_retx, dest_addr, event->timestamp_us,
                                        stdout, src->id);
            }
        }

        sim_event_t retry = *event;
        retry.timestamp_us += 1500000ULL;
        retry.data.node.y = (float)(retry_count + 1);
        event_queue_push(events, &retry);
        return;
    }

    /* Route exists — build and send DATA packet */

    /* Reactive: forward_data overwrites this from the route. Flood transport:
     * originate at the operator-settable flood hop budget. */
    uint8_t hop_limit = flood_origination_hop_limit(g_flood_transport_enabled, g_flood_hop_limit);
    forward_result_t fwd_res;
    if (g_flood_transport_enabled) {
        /* Flood origination: no route to resolve. Send at the flood hop budget
         * and broadcast; header.dest_addr stays D so only D delivers while
         * every relay floods it onward. */
        memset(&fwd_res, 0, sizeof(fwd_res));
        fwd_res.should_send = true;
        fwd_res.next_hop = 0xFFFFFFFF;
    } else {
        fwd_res = forward_data(&src->routes, dest_addr, &hop_limit, now_ms);
    }
    if (!fwd_res.should_send)
        return;

    /* Determine payload size from x field (Phase 4: 0 = header-only, >0 = with payload) */
    int payload_size = (int)event->data.node.x;
    if (payload_size < 0)
        payload_size = 0;

    bramble_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = BRAMBLE_VERSION;
    hdr.type = PKT_TYPE_DATA;
    hdr.flags = 0;
    hdr.hop_limit = hop_limit;
    hdr.dest_addr = dest_addr;
    hdr.packet_id = pcg32_random(rng);

    /* Build payload */
    uint8_t payload_buf[1024];
    for (int i = 0; i < payload_size && i < (int)sizeof(payload_buf); i++) {
        payload_buf[i] = (uint8_t)(pcg32_random(rng) & 0xFF);
    }
    if (payload_size > (int)sizeof(payload_buf))
        payload_size = (int)sizeof(payload_buf);

    /* Check if fragmentation needed (Phase 4) */
    if (payload_size > FRAG_MAX_PLAINTEXT) {
        /* Fragment the payload */
        fragment_t frags[FRAG_MAX_FRAGMENTS];
        uint16_t msg_id = (uint16_t)(hdr.packet_id & 0xFFFF);
        int num_frags =
            fragment_split(payload_buf, (size_t)payload_size, msg_id, frags, FRAG_MAX_FRAGMENTS);
        if (num_frags <= 0)
            return;

        bridge_msg_track_add(msg_track, msg_track_count, hdr.packet_id, src->addr, dest_addr,
                             event->timestamp_us);

        /* Pre-generate fragment packet_ids and track them all */
        uint32_t frag_pids[FRAG_MAX_FRAGMENTS];
        frag_pids[0] = hdr.packet_id;
        for (int fi = 1; fi < num_frags; fi++) {
            frag_pids[fi] = pcg32_random(rng);
            bridge_msg_track_add(msg_track, msg_track_count, frag_pids[fi], src->addr, dest_addr,
                                 event->timestamp_us);
        }

        for (int fi = 0; fi < num_frags; fi++) {
            bramble_header_t fhdr = hdr;
            fhdr.packet_id = frag_pids[fi];

            uint8_t data_buf[256];
            bramble_header_serialize(&fhdr, data_buf, HEADER_SIZE);

            /* Encrypt fragment payload (Phase 5) */
            uint8_t* frag_payload = frags[fi].data;
            size_t frag_len = frags[fi].len;
            uint8_t enc_buf[256];
            size_t enc_offset = HEADER_SIZE;

            uint8_t key[BRAMBLE_KEY_SIZE];
            derive_pair_key(src->addr, dest_addr, key);
            uint8_t nonce[BRAMBLE_NONCE_SIZE];
            sim_build_nonce(src->addr, src->crypto_counter++, nonce);
            /* AAD binds this fragment's header (hop_limit masked) */
            uint8_t aad[HEADER_SIZE];
            bramble_header_build_aad(&fhdr, aad, sizeof(aad));
            memcpy(enc_buf, data_buf, HEADER_SIZE);
            memcpy(enc_buf + enc_offset, nonce, BRAMBLE_NONCE_SIZE);
            enc_offset += BRAMBLE_NONCE_SIZE;
            uint8_t tag[BRAMBLE_TAG_SIZE];
            if (crypto_aes256gcm_encrypt(key, nonce, frag_payload, frag_len, aad, HEADER_SIZE,
                                         enc_buf + enc_offset + BRAMBLE_TAG_SIZE, tag) == 0) {
                memcpy(enc_buf + enc_offset, tag, BRAMBLE_TAG_SIZE);
                enc_offset += BRAMBLE_TAG_SIZE + frag_len;
                metrics->crypto_encrypted++;
            } else {
                /* Fallback: send unencrypted */
                memcpy(enc_buf + HEADER_SIZE, frag_payload, frag_len);
                enc_offset = HEADER_SIZE + frag_len;
            }

            /* Airtime check (Phase 3): real time-on-air for this fragment */
            uint32_t frag_toa_ms = radio_frame_airtime_ms(radio, (uint16_t)enc_offset);
            airtime_budget_set_mesh_size(&src->airtime, (uint8_t)neighbor_count(&src->neighbors));
            if (!airtime_budget_can_transmit(&src->airtime, MSG_TIER_NORMAL, frag_toa_ms)) {
                metrics->airtime_deferred++;
                fprintf(stdout,
                        "{\"type\":\"airtime_exceeded\",\"timestamp_us\":%llu,\"node\":\"%s\"}\n",
                        (unsigned long long)event->timestamp_us, src->id);
                fflush(stdout);
                continue;
            }

            outbound_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            memcpy(pkt.data, enc_buf, enc_offset);
            pkt.len = (uint16_t)enc_offset;
            /* Flooding F1 Task 3: flood origination broadcasts (dest
             * 0xFFFFFFFF); reactive sends to the resolved next hop. */
            pkt.is_broadcast = g_flood_transport_enabled;
            pkt.dest_addr = g_flood_transport_enabled ? 0xFFFFFFFF : fwd_res.next_hop;
            pkt.pkt_type = PKT_TYPE_DATA;

            airtime_budget_debit(&src->airtime, MSG_TIER_NORMAL, frag_toa_ms);
            sim_radio_broadcast(src, &pkt, nodes, radio, rng, events, metrics, event->timestamp_us);
            metrics->fragments_sent++;
        }

        /* Add to pending acks */
        pending_ack_add(&src->pending_acks, hdr.packet_id, dest_addr, MSG_TIER_NORMAL, payload_buf,
                        (uint16_t)payload_size, now_ms);

        src->packets_originated++;
        metrics_record_message_sent(metrics);
        fprintf(stdout,
                "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"packet_id\":\"0x%08X\""
                ",\"fragments\":%d}\n",
                (unsigned long long)event->timestamp_us, src->id, dest_addr, hdr.packet_id,
                num_frags);
        fflush(stdout);
        return;
    }

    /* Non-fragmented path */
    uint8_t data_buf[256];
    bramble_header_serialize(&hdr, data_buf, HEADER_SIZE);
    uint16_t total_len = HEADER_SIZE;

    if (payload_size > 0) {
        /* Encrypt payload (Phase 5) */
        uint8_t key[BRAMBLE_KEY_SIZE];
        derive_pair_key(src->addr, dest_addr, key);
        uint8_t nonce[BRAMBLE_NONCE_SIZE];
        sim_build_nonce(src->addr, src->crypto_counter++, nonce);
        /* AAD binds the header (hop_limit masked), matching firmware */
        uint8_t aad[HEADER_SIZE];
        bramble_header_build_aad(&hdr, aad, sizeof(aad));

        memcpy(data_buf + total_len, nonce, BRAMBLE_NONCE_SIZE);
        total_len += BRAMBLE_NONCE_SIZE;

        uint8_t tag[BRAMBLE_TAG_SIZE];
        uint8_t ciphertext[256];
        if (crypto_aes256gcm_encrypt(key, nonce, payload_buf, (size_t)payload_size, aad,
                                     HEADER_SIZE, ciphertext, tag) == 0) {
            memcpy(data_buf + total_len, tag, BRAMBLE_TAG_SIZE);
            total_len += BRAMBLE_TAG_SIZE;
            memcpy(data_buf + total_len, ciphertext, (size_t)payload_size);
            total_len += (uint16_t)payload_size;
            metrics->crypto_encrypted++;
        } else {
            /* Fallback: send unencrypted */
            total_len = HEADER_SIZE;
            memcpy(data_buf + total_len, payload_buf, (size_t)payload_size);
            total_len += (uint16_t)payload_size;
        }
    }

    /* Airtime check (Phase 3): real time-on-air for this frame */
    uint32_t toa_ms = radio_frame_airtime_ms(radio, total_len);
    airtime_budget_set_mesh_size(&src->airtime, (uint8_t)neighbor_count(&src->neighbors));
    if (!airtime_budget_can_transmit(&src->airtime, MSG_TIER_NORMAL, toa_ms)) {
        metrics->airtime_deferred++;
        fprintf(stdout, "{\"type\":\"airtime_exceeded\",\"timestamp_us\":%llu,\"node\":\"%s\"}\n",
                (unsigned long long)event->timestamp_us, src->id);
        fflush(stdout);
        /* Reschedule after refill */
        sim_event_t retry = *event;
        retry.timestamp_us += 1000000ULL;
        retry.data.node.y = (float)(retry_count + 1);
        event_queue_push(events, &retry);
        return;
    }

    bridge_msg_track_add(msg_track, msg_track_count, hdr.packet_id, src->addr, dest_addr,
                         event->timestamp_us);

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, data_buf, total_len);
    pkt.len = total_len;
    /* Flooding F1 Task 3: flood origination broadcasts (dest 0xFFFFFFFF) so
     * every relay floods it; reactive sends to the resolved next hop. */
    pkt.is_broadcast = g_flood_transport_enabled;
    pkt.dest_addr = g_flood_transport_enabled ? 0xFFFFFFFF : fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;
    src->packets_originated++;

    /* Pending ACK (Phase 1) */
    pending_ack_add(&src->pending_acks, hdr.packet_id, dest_addr, MSG_TIER_NORMAL, pkt.data,
                    pkt.len, now_ms);
    airtime_budget_debit(&src->airtime, MSG_TIER_NORMAL, toa_ms);

    sim_radio_broadcast(src, &pkt, nodes, radio, rng, events, metrics, event->timestamp_us);

    metrics_record_message_sent(metrics);
    fprintf(stdout,
            "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"packet_id\":\"0x%08X\"}\n",
            (unsigned long long)event->timestamp_us, src->id, dest_addr, hdr.packet_id);
    fflush(stdout);
}

/* ─── Retransmission handler (Phase 1) ─────────────────────────────────── */

void bridge_handle_retransmit(sim_node_t* node, node_array_t* nodes, radio_config_t* radio,
                              pcg32_state_t* rng, event_queue_t* events, metrics_state_t* metrics,
                              uint64_t now_us) {
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* Phase 6: Periodic maintenance for extended node state */
    {
        int node_idx = (int)(node - nodes->nodes);
        bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
        if (ext) {
            /* Mailbox: purge expired entries (24h TTL) */
            int before = ext->mailbox.count;
            mailbox_purge_expired(&ext->mailbox, now_ms);
            int purged = before - ext->mailbox.count;
            if (purged > 0) {
                g_ext_metrics.mailbox_expired += (uint64_t)purged;
            }

            /* Location: update sim position from node's current x/y coordinates */
            node_ext_set_sim_position(ext, node->x, node->y);
        }
    }

    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        pending_ack_t* pa = &node->pending_acks.entries[i];
        if (!pa->active)
            continue;
        if (pa->attempt == 0)
            continue; /* not yet due for retry */
        if (pa->next_retry_ms > now_ms)
            continue;

        /* This entry needs retransmission */
        if (pa->attempt >= pa->max_attempts) {
            /* Exhausted retries — remove and count as failed */
            pa->active = false;
            continue;
        }

        /* Retransmit. Flooding F1 Task 3: a flood-originated pending entry
         * retries by RE-FLOODING the stored broadcast frame (same packet_id),
         * NOT by a routed retransmit. forward_data needs a route (none exist
         * under flood), so bypass it and re-broadcast at the full hop budget;
         * the destination's re-ACK-on-duplicate gives another confirmation
         * chance. Reactive (toggle off) is unchanged. */
        uint8_t hop_limit =
            flood_origination_hop_limit(g_flood_transport_enabled, g_flood_hop_limit);
        forward_result_t fwd_res;
        if (g_flood_transport_enabled) {
            memset(&fwd_res, 0, sizeof(fwd_res));
            fwd_res.should_send = true;
            fwd_res.next_hop = 0xFFFFFFFF;
        } else {
            fwd_res = forward_data(&node->routes, pa->dest_addr, &hop_limit, now_ms);
        }
        if (!fwd_res.should_send)
            continue;

        /* Airtime check: real time-on-air of the stored frame */
        uint32_t retx_toa_ms = radio_frame_airtime_ms(radio, pa->packet_len);
        airtime_budget_set_mesh_size(&node->airtime, (uint8_t)neighbor_count(&node->neighbors));
        if (!airtime_budget_can_transmit(&node->airtime, MSG_TIER_NORMAL, retx_toa_ms)) {
            metrics->airtime_deferred++;
            continue;
        }

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        memcpy(pkt.data, pa->packet_data, pa->packet_len);
        pkt.data[3] = hop_limit; /* update hop_limit */
        pkt.len = pa->packet_len;
        pkt.is_broadcast = g_flood_transport_enabled;
        pkt.dest_addr = g_flood_transport_enabled ? 0xFFFFFFFF : fwd_res.next_hop;
        pkt.pkt_type = PKT_TYPE_DATA;

        airtime_budget_debit(&node->airtime, MSG_TIER_NORMAL, retx_toa_ms);
        sim_radio_broadcast(node, &pkt, nodes, radio, rng, events, metrics, now_us);
        metrics->messages_retried++;

        /* Schedule next retry */
        pa->next_retry_ms = now_ms + tier_base_delay_ms(pa->tier) * (1 << pa->attempt);
        pa->attempt++;

        fprintf(stdout,
                "{\"type\":\"message_retransmit\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"packet_id\":\"0x%08X\",\"attempt\":%d}\n",
                (unsigned long long)now_us, node->id, pa->packet_id, pa->attempt);
        fflush(stdout);
    }
}

/* ─── Identity attestation origination (per-node identity Phase 3) ───── */
/*
 * Mirrors firmware's send_identity_attestation (main/mesh_task.c) step for
 * step, order included: canonical message -> Ed25519 sign -> seq draw ->
 * ident_relay_sign (the MAC covers sig and seq) -> serialize -> one
 * BROADCAST-lane budget-gated transmission. event->data.node.addr != 0
 * claims that address instead of the node's own: the frame is then a keyed
 * insider's impersonation attempt, internally valid (its Ed25519 sig
 * verifies against its OWN embedded key) but bound to someone else's
 * address; post-Phase-4 EVERY receiver refuses it at the addr<->key check
 * (identity_addr_mismatch), pin or no pin. event->data.node.x != 0
 * rotates the node's X25519 pub before attesting: the frame then passes
 * the addr check (same Ed key) but re-binds the address to a different
 * X25519 key, which receivers holding the original pin refuse as a TOFU
 * CONFLICT (the DM-continuity red flag).
 */
void bridge_handle_generate_attestation(sim_event_t* event, node_array_t* nodes,
                                        radio_config_t* radio, pcg32_state_t* rng,
                                        event_queue_t* events, metrics_state_t* metrics) {
    sim_node_t* src = node_array_find_by_id(nodes, event->data.node.node_id);
    if (!src || !src->active)
        return;
    int node_idx = (int)(src - nodes->nodes);
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (!ext)
        return;

    /* Mandatory-provisioning (Task 2): an unprovisioned node is INERT -- the
     * relay-gate MAC needs the network key, so it emits no attestation. */
    if (!ext->provisioned) {
        fprintf(stdout,
                "{\"type\":\"unprovisioned_inert\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"frame\":\"attestation\"}\n",
                (unsigned long long)event->timestamp_us, src->id);
        fflush(stdout);
        return;
    }

    uint32_t claimed = event->data.node.addr ? event->data.node.addr : src->addr;

    /* Scripted X25519 rotation (see the doc comment above): mutate the
     * PERSISTENT pattern so subsequent attestations keep the rotated key,
     * exactly like a node whose DM key really changed. */
    if (event->data.node.x != 0) {
        for (int i = 0; i < 32; i++) {
            ext->ident_x25519_pub[i] ^= 0xA5;
        }
    }

    bramble_identity_attestation_t att;
    memset(&att, 0, sizeof(att));
    att.header.version = BRAMBLE_VERSION;
    att.header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    att.header.flags = 0;
    att.header.hop_limit =
        flood_origination_hop_limit(g_flood_transport_enabled, g_flood_hop_limit);
    att.header.dest_addr = 0xFFFFFFFF;
    att.header.packet_id = pcg32_random(rng);
    att.src_addr = claimed;
    memcpy(att.x25519_pub, ext->ident_x25519_pub, 32);
    memcpy(att.ed25519_pub, src->ident_ed_pub, 32);

    /* Trust-anchor campaign (P2): carry the fleet-anchor endorsement cert when
     * this node is endorsed (the default). The offline anchor holder's job,
     * done host-side: sign the P0 canonical endorsement message over this
     * node's real Ed25519 key with not_after=PERMANENT using the test anchor
     * private key. An unendorsed node leaves not_after=0 (no cert), so anchored
     * receivers refuse to pin it. Firmware fills these fields from
     * identity_endorsement_get; gosim signs per-node because the process-global
     * cert store cannot hold a distinct cert per simulated node. */
    if (ext->endorsed) {
        uint8_t emsg[IDENTITY_ENDORSEMENT_MSG_SIZE];
        if (identity_endorsement_msg(src->ident_ed_pub, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT,
                                     emsg, sizeof(emsg)) == IDENTITY_ENDORSEMENT_MSG_SIZE &&
            crypto_ed25519_sign(g_bridge_anchor_priv, emsg, sizeof(emsg), att.endorsement_sig) ==
                0) {
            att.not_after = IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT;
        }
    }

    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    if (bramble_identity_attestation_signed_msg(&att, msg, sizeof(msg)) != ESP_OK)
        return;
    if (crypto_ed25519_sign(src->ident_ed_priv, msg, sizeof(msg), att.sig) != 0)
        return;

    /* Fresh origin seq per origination (mirrors control_seq_next), written
     * before the MAC since the MAC covers it. */
    uint64_t seq = ++ext->ident_seq;
    att.seq[0] = (uint8_t)(seq >> 40);
    att.seq[1] = (uint8_t)(seq >> 32);
    att.seq[2] = (uint8_t)(seq >> 24);
    att.seq[3] = (uint8_t)(seq >> 16);
    att.seq[4] = (uint8_t)(seq >> 8);
    att.seq[5] = (uint8_t)seq;
    ident_relay_sign(&att);

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    if (bramble_identity_attestation_serialize(&att, pkt.data, sizeof(pkt.data)) != ESP_OK)
        return;
    pkt.len = IDENTITY_ATTESTATION_SIZE;
    pkt.is_broadcast = true;
    pkt.dest_addr = 0xFFFFFFFF;
    pkt.pkt_type = PKT_TYPE_IDENTITY_ATTESTATION;

    /* tx_gate_kind_tier: TX_KIND_DATA_BROADCAST -> AIRTIME_TIER_BROADCAST,
     * the same lane firmware's attestation TX debits. */
    if (budget_gated_send(src, &pkt, AIRTIME_TIER_BROADCAST, nodes, radio, rng, events, metrics,
                          event->timestamp_us)) {
        src->packets_originated++;
        fprintf(stdout,
                "{\"type\":\"attestation_sent\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"addr\":\"0x%08X\",\"packet_id\":\"0x%08X\""
                ",\"ed8\":\"%02X%02X%02X%02X\"}\n",
                (unsigned long long)event->timestamp_us, src->id, claimed, att.header.packet_id,
                att.ed25519_pub[0], att.ed25519_pub[1], att.ed25519_pub[2], att.ed25519_pub[3]);
        fflush(stdout);
    }
}

/* --- Runtime anchor provisioning (P2 red-team) --------------------------- */
/*
 * Scripted "provision_anchor" event: the sim analog of an operator running
 * bramble.setAnchor mid-life to harden the fleet without a reboot. (Re-)anchors
 * the named node to the fleet test anchor via the REAL identity_store_set_anchor
 * -- so if the node was un-anchored ("unanchored": true) its stale TOFU pins are
 * DROPPED, exactly like firmware's mesh_set_pin_anchor path. Emits how many pins
 * were dropped so a scenario can assert the hardening actually purged them.
 */
void bridge_handle_provision_anchor(sim_event_t* event, node_array_t* nodes) {
    sim_node_t* node = node_array_find_by_id(nodes, event->data.node.node_id);
    if (!node)
        return;
    int node_idx = (int)(node - nodes->nodes);
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (!ext)
        return;

    int before = identity_store_count(&ext->ident_pins);
    identity_store_set_anchor(&ext->ident_pins, g_bridge_anchor_pub);
    int after = identity_store_count(&ext->ident_pins);

    fprintf(stdout,
            "{\"type\":\"anchor_provisioned\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"dropped_pins\":%d}\n",
            (unsigned long long)event->timestamp_us, node->id, before - after);
    fflush(stdout);
}

/* ─── Node join extended initializer ────────────────────────────────────── */
void bridge_handle_node_join_ext(int node_idx, uint32_t addr, float x, float y, uint64_t now_us) {
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (!ext)
        return;

    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* Set initial simulated position from node coordinates */
    node_ext_set_sim_position(ext, x, y);

    /* Per-node identity Phase 3/4: the Ed25519 keypair itself lives on
     * sim_node_t (created once at node_array_add, the NVS-persistence
     * analog; node->addr derives from it, the Phase 4 rebind). FIRST join
     * initializes the remaining identity state here; a rejoin keeps it
     * (resetting the X25519 pattern or seq would make a rejoin look like
     * an impersonation / replay to its peers). The X25519 pub is a
     * deterministic pattern off the address: gosim does not model the DM
     * key exchange, and the attestation binds whatever bytes are
     * attested. */
    if (!ext->ident_initialized) {
        for (int i = 0; i < 32; i++) {
            ext->ident_x25519_pub[i] = (uint8_t)((addr >> ((i % 4) * 8)) ^ (uint8_t)i);
        }
        ext->ident_seq = 0;
        identity_store_init(&ext->ident_pins, now_ms);
        /* Trust-anchor campaign (P2): every node is ANCHORED to the fixed test
         * anchor, so it pins ONLY endorsed identities (bridge_init endorses the
         * whole fleet by default, so scenarios still mesh). This is the sim
         * analog of a firmware node that loaded a provisioned anchor at boot. */
        identity_store_set_anchor(&ext->ident_pins, g_bridge_anchor_pub);
        ext->ident_initialized = true;
        /* Mandatory-provisioning (Task 2): a freshly joined node holds the
         * fleet key by default. A scenario opts a node out via
         * bridge_node_set_provisioned(idx, false) AFTER this join. Kept in the
         * first-init guard so a rejoin preserves an intentionally-inert node. */
        ext->provisioned = true;
        /* Trust-anchor campaign (P2): endorsed by default; a scenario opts a
         * node out via bridge_node_set_endorsed(idx, false) AFTER this join.
         * In the first-init guard so a rejoin preserves the setting. */
        ext->endorsed = true;
    }

    fprintf(stdout,
            "{\"type\":\"node_ext_initialized\",\"timestamp_us\":%llu"
            ",\"node_idx\":%d,\"addr\":\"0x%08X\""
            ",\"lat_e7\":%d,\"lon_e7\":%d}\n",
            (unsigned long long)now_us, node_idx, addr, ext->location.my_position.latitude_e7,
            ext->location.my_position.longitude_e7);
    fflush(stdout);
}

/* ─── Duty-cycle cap (DES-8, Task 5) ────────────────────────────────────── */
void bridge_apply_duty_cycle_cap(sim_node_t* node, uint8_t max_duty_cycle_pct) {
    airtime_budget_set_duty_cap(&node->airtime, max_duty_cycle_pct, true);
}

/* ─── Init relay path tracker + extended state ────────────────────────── */
void bridge_init(void) {
    /* Flooding F1 Task 1 fix: sim_emitter's g_emitter_quiet is a process-wide
     * global (engine/sim_emitter.c), never reset anywhere else. radio_
     * harness.go's newRadioHarness() sets it true for the low-level radio-
     * model tests (collision/budget/duty-cycle/etc.) and never restores it;
     * since Go test binaries run every test in one process, ANY later
     * protocol-bridge test (this file's normal path, used by every Sim())
     * that happened to run after one of those in test order silently lost
     * packet_sent/packet_received/route_added/packet_dropped/route_removed
     * events for the rest of the process, with no per-test symptom other
     * than "these events are just missing" -- exactly what broke
     * TestFloodTransportRelaysAsBroadcastNotUnicastForward the first time it
     * ran as part of the full suite (passed alone, failed after an earlier
     * radioHarness-based test). bridge_init() runs at the start of every
     * NewSim(), so resetting it here makes every protocol-bridge sim start
     * from "verbose", independent of what any earlier radioHarness test left
     * behind; radioHarness itself is unaffected (it always sets quiet=true
     * itself in newRadioHarness(), regardless of this call). */
    sim_emitter_set_quiet(false);

    relay_path_init();

    /* Mandatory-provisioning (Task 2): provision the shared default network key
     * for the whole sim fleet BEFORE any node originates, so control-plane
     * MACs (routing_auth / discovery / attestation relay gate) sign and verify
     * exactly as a provisioned firmware node would. Without this every sim
     * node would be inert and no scenario would mesh. */
    network_key_set_provisioned(BRIDGE_DEFAULT_NET_KEY);

    /* Trust-anchor campaign (P2): expand the fixed test anchor seed into the
     * fleet anchor keypair BEFORE any node joins (join sets it on the pin store
     * and attestation TX signs the per-node cert with it). identity_anchor_set
     * records the PUBLIC key in the process-global module memory too, mirroring
     * a provisioned firmware node (harmless; the pin gate reads the per-node
     * store's copy, set at join). */
    crypto_ed25519_keypair_from_seed(BRIDGE_TEST_ANCHOR_SEED, g_bridge_anchor_pub,
                                     g_bridge_anchor_priv);
    identity_anchor_set(g_bridge_anchor_pub);

    /* Phase 6: Initialize all per-node extended state */
    bridge_node_ext_init_all();

    /* Phase 6: Initialize public channel (Channel 0, well-known PSK) */
    g_num_pub_channels = 1;
    int ret = public_channel_init(g_pub_channels, &g_num_pub_channels);
    if (ret == 0) {
        fprintf(stdout,
                "{\"type\":\"public_channel_init\""
                ",\"channel_id\":%d,\"epoch\":%d}\n",
                g_pub_channels[0].channel_id, g_pub_channels[0].epoch);
        fflush(stdout);
    }
}
