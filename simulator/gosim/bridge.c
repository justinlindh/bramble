#include "bridge.h"
#include "../../components/packet/include/packet.h"
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include "../../components/routing/include/forwarding.h"
#include "../../components/airtime/include/airtime_budget.h"
#include "../../components/fragment/include/fragment.h"
#include "../../components/crypto/include/crypto.h"
/* Note: mailbox.h, location.h, group.h,
 * channel_key.h, public_channel.h are all pulled in transitively via
 * bridge.h (Phase 6 headers). */

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ─── Extended per-node state (Phase 6) ─────────────────────────────────── */

static bridge_node_ext_t g_node_ext[MAX_NODES];
static bridge_ext_metrics_t g_ext_metrics;

/* Public channel state (one global instance) */
static bramble_channel_t g_pub_channels[16];
static int g_num_pub_channels = 0;

bridge_node_ext_t* bridge_node_ext_get(int node_idx) {
    if (node_idx < 0 || node_idx >= MAX_NODES)
        return NULL;
    return &g_node_ext[node_idx];
}

bridge_ext_metrics_t* bridge_ext_metrics_get(void) { return &g_ext_metrics; }

void bridge_node_ext_init_all(void) {
    memset(&g_node_ext, 0, sizeof(g_node_ext));
    memset(&g_ext_metrics, 0, sizeof(g_ext_metrics));
    for (int i = 0; i < MAX_NODES; i++) {
        mailbox_init(&g_node_ext[i].mailbox);
        location_init(&g_node_ext[i].location);
        group_init(&g_node_ext[i].group);
        g_node_ext[i].initialized = true;
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

/* Real time-on-air for a frame, in ms, for airtime budget accounting.
 * Matches what the radio model actually occupies the medium with. */
static uint32_t frame_airtime_ms(const radio_config_t* radio, uint16_t frame_len) {
    uint32_t us = radio_frame_airtime_us(radio, frame_len);
    uint32_t ms = (us + 999u) / 1000u;
    return ms ? ms : 1u;
}

/* ─── Global simulation time ───────────────────────────────────────────── */
uint64_t g_bridge_sim_time_us = 0;

void bridge_set_sim_time(uint64_t us) { g_bridge_sim_time_us = us; }

uint32_t sim_get_time_ms(void) { return (uint32_t)(g_bridge_sim_time_us / 1000); }

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

/* ─── Internal packet handlers ─────────────────────────────────────────── */

static void _handle_beacon(sim_node_t* rx, const uint8_t* buf, uint16_t len, int8_t rssi,
                           uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                           node_anomaly_tracker_t* anomaly) {
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, buf, len) != ESP_OK)
        return;

    neighbor_update(&rx->neighbors, beacon.src_addr, rssi, 0, beacon.pubkey_hash, now_ms);

    route_entry_t* existing = route_lookup(&rx->routes, beacon.src_addr);
    bool new_or_broken = (!existing || existing->state == ROUTE_BROKEN);

    /* Direct-neighbor route from the beacon, scored by link quality alone,
     * matching the firmware's penalty-based metric (the composite scoring
     * module was deleted as decorative; DES-4). */
    int node_idx = (int)(rx - nodes->nodes);
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    uint8_t metric = metric_apply_link_penalty(255, rssi, 0);

    route_install(&rx->routes, beacon.src_addr, beacon.src_addr, 1, metric, ROUTE_ACTIVE, now_ms);

    if (new_or_broken) {
        emit_route_added(stdout, now_us, rx->id, beacon.src_addr, beacon.src_addr, 1);

        anomaly_check_route_flap(&anomaly[node_idx].flap, beacon.src_addr, beacon.src_addr, now_us,
                                 stdout, rx->id);
    }

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

        sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
    } else if (rreq.header.hop_limit > 1) {
        bramble_rreq_t fwd = rreq_forward(&rreq, rx->addr, rssi, 0);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rreq_serialize(&fwd, pkt.data, RREQ_SIZE);
        pkt.len = RREQ_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr = 0xFFFFFFFF;
        pkt.pkt_type = PKT_TYPE_RREQ;
        rx->packets_forwarded++;

        {
            int node_idx = (int)(rx - nodes->nodes);
            anomaly_check_rreq_retx(&anomaly[node_idx].rreq_retx, rreq.header.dest_addr, now_us,
                                    stdout, rx->id);
        }

        /* Jittered rebroadcast (DES-3), same 50-300ms window as firmware,
         * so same-hop relays do not key up at the same instant. */
        uint64_t jitter_us = (uint64_t)discovery_forward_jitter_ms(pcg32_random(rng)) * 1000ULL;
        sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us + jitter_us);
    }
}

static void _handle_rrep(sim_node_t* rx, const uint8_t* buf, uint16_t len, uint32_t pkt_src_addr,
                         uint64_t now_us, uint32_t now_ms, node_array_t* nodes,
                         radio_config_t* radio, pcg32_state_t* rng, event_queue_t* events,
                         metrics_state_t* metrics, node_anomaly_tracker_t* anomaly) {
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, buf, len) != ESP_OK)
        return;

    route_install(&rx->routes, rrep.src_addr, pkt_src_addr, rrep.hop_count, rrep.route_metric,
                  ROUTE_ACTIVE, now_ms);
    emit_route_added(stdout, now_us, rx->id, rrep.src_addr, pkt_src_addr, rrep.hop_count);

    int node_idx = (int)(rx - nodes->nodes);
    anomaly_check_route_flap(&anomaly[node_idx].flap, rrep.src_addr, pkt_src_addr, now_us, stdout,
                             rx->id);

    pending_discovery_t* pd = discovery_lookup_by_query(&rx->pending_discoveries, rrep.query_id);
    if (pd) {
        discovery_remove(&rx->pending_discoveries, pd->dest_addr);
        return;
    }

    reverse_route_t* rr = reverse_route_lookup(&rx->reverse_routes, rrep.query_id);
    if (!rr)
        return;

    bramble_rrep_t fwd = rrep_forward(&rrep, rr->prev_hop);

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    bramble_rrep_serialize(&fwd, pkt.data, RREP_SIZE);
    pkt.len = RREP_SIZE;
    pkt.is_broadcast = false;
    pkt.dest_addr = rr->prev_hop;
    pkt.pkt_type = PKT_TYPE_RREP;
    rx->packets_forwarded++;

    sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
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
        /* Always clear pending ack and flow control on receipt */
        pending_ack_remove(&rx->pending_acks, receipt.orig_packet_id);
        flow_on_ack(&rx->flow_control, receipt.src_addr);

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
    rx->packets_forwarded++;

    sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
}

static void _handle_data(sim_node_t* rx, const uint8_t* buf, uint16_t len, uint64_t now_us,
                         uint32_t now_ms, node_array_t* nodes, radio_config_t* radio,
                         pcg32_state_t* rng, event_queue_t* events, metrics_state_t* metrics,
                         node_anomaly_tracker_t* anomaly, msg_tracker_t* msg_track,
                         int msg_track_count) {
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK)
        return;

    int node_idx = (int)(rx - nodes->nodes);

    anomaly_record_rx(&anomaly[node_idx].blackhole, now_us);
    anomaly_check_loop(&anomaly[node_idx].loop, hdr.packet_id, now_us, stdout, rx->id);

    if (hdr.dest_addr == rx->addr) {
        /* Message reached destination — send delivery receipt back to source */
        uint32_t orig_sender = bridge_msg_track_find_src(msg_track, msg_track_count, hdr.packet_id);

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
            receipt.header.hop_limit = ROUTE_HOP_LIMIT_MAX; /* firmware receipt budget */
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

                    sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
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

        sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
        emit_packet_dropped(stdout, now_us, rx->id, "no_route");

        /* Phase 6: Mailbox — store the DATA payload for the offline destination.
         * This relay node volunteers to hold the message until the dest rejoins. */
        bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
        if (ext && len > HEADER_SIZE) {
            /* Store the raw packet payload (everything after the header) */
            uint32_t orig_src =
                bridge_msg_track_find_src(msg_track, msg_track_count, hdr.packet_id);
            if (orig_src != 0) {
                int stored =
                    mailbox_store(&ext->mailbox, orig_src, hdr.dest_addr, buf + HEADER_SIZE,
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
    rx->packets_forwarded++;

    anomaly_record_fwd(&anomaly[node_idx].blackhole, now_us);

    sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
}

/* ─── Public packet handling wrappers ──────────────────────────────────── */

void bridge_handle_receive_packet(sim_event_t* event, node_array_t* nodes, radio_config_t* radio,
                                  pcg32_state_t* rng, event_queue_t* events,
                                  metrics_state_t* metrics, node_anomaly_tracker_t* anomaly,
                                  msg_tracker_t* msg_track, int msg_track_count) {
    sim_node_t* rx = node_array_find_by_addr(nodes, event->data.packet.dest_addr);
    if (!rx || !rx->active)
        return;

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
        _handle_data(rx, buf, len, event->timestamp_us, now_ms, nodes, radio, rng, events, metrics,
                     anomaly, msg_track, msg_track_count);
        break;
    case PKT_TYPE_DELIVERY_RECEIPT:
        _handle_delivery_receipt(rx, buf, len, event->timestamp_us, now_ms, nodes, radio, rng,
                                 events, metrics, anomaly, msg_track, msg_track_count);
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

    uint32_t dest_addr = event->data.node.addr;
    uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);

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

    route_entry_t* route = route_lookup(&src->routes, dest_addr);

    if (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING) {
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
            uint32_t query_id = pcg32_random(rng);
            discovery_start(&src->pending_discoveries, dest_addr, query_id, now_ms);
            should_send_rreq = true;
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

            sim_radio_broadcast(src, &pkt, nodes, radio, rng, events, metrics, event->timestamp_us);

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

    /* Flow control check (Phase 1) */
    if (!flow_can_send(&src->flow_control, dest_addr)) {
        /* Window full — reschedule */
        sim_event_t retry = *event;
        retry.timestamp_us += 500000ULL; /* retry in 500ms */
        event_queue_push(events, &retry);
        return;
    }

    uint8_t hop_limit = ROUTE_HOP_LIMIT_MAX; /* firmware DATA hop budget */
    forward_result_t fwd_res = forward_data(&src->routes, dest_addr, &hop_limit, now_ms);
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
            uint32_t frag_toa_ms = frame_airtime_ms(radio, (uint16_t)enc_offset);
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
            pkt.is_broadcast = false;
            pkt.dest_addr = fwd_res.next_hop;
            pkt.pkt_type = PKT_TYPE_DATA;

            airtime_budget_debit(&src->airtime, MSG_TIER_NORMAL, frag_toa_ms);
            sim_radio_broadcast(src, &pkt, nodes, radio, rng, events, metrics, event->timestamp_us);
            metrics->fragments_sent++;
        }

        /* Add to pending acks and flow control */
        pending_ack_add(&src->pending_acks, hdr.packet_id, dest_addr, MSG_TIER_NORMAL, payload_buf,
                        (uint16_t)payload_size, now_ms);
        flow_on_send(&src->flow_control, dest_addr);

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
    uint32_t toa_ms = frame_airtime_ms(radio, total_len);
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
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;
    src->packets_originated++;

    /* Pending ACK + flow control (Phase 1) */
    pending_ack_add(&src->pending_acks, hdr.packet_id, dest_addr, MSG_TIER_NORMAL, pkt.data,
                    pkt.len, now_ms);
    flow_on_send(&src->flow_control, dest_addr);
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
            flow_on_failure(&node->flow_control, pa->dest_addr);
            pa->active = false;
            continue;
        }

        /* Retransmit */
        uint8_t hop_limit = ROUTE_HOP_LIMIT_MAX; /* firmware DATA hop budget */
        forward_result_t fwd_res = forward_data(&node->routes, pa->dest_addr, &hop_limit, now_ms);
        if (!fwd_res.should_send)
            continue;

        /* Airtime check: real time-on-air of the stored frame */
        uint32_t retx_toa_ms = frame_airtime_ms(radio, pa->packet_len);
        if (!airtime_budget_can_transmit(&node->airtime, MSG_TIER_NORMAL, retx_toa_ms)) {
            metrics->airtime_deferred++;
            continue;
        }

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        memcpy(pkt.data, pa->packet_data, pa->packet_len);
        pkt.data[3] = hop_limit; /* update hop_limit */
        pkt.len = pa->packet_len;
        pkt.is_broadcast = false;
        pkt.dest_addr = fwd_res.next_hop;
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

/* ─── Node join extended initializer ────────────────────────────────────── */
void bridge_handle_node_join_ext(int node_idx, uint32_t addr, float x, float y, uint64_t now_us) {
    bridge_node_ext_t* ext = bridge_node_ext_get(node_idx);
    if (!ext)
        return;

    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* Set initial simulated position from node coordinates */
    node_ext_set_sim_position(ext, x, y);

    /* Emergency: ensure state machine is reset for this node */

    /* Group: create a default sim group for testing (first 4 nodes only) */
    if (node_idx < 4) {
        /* Nodes 0-3 share a sim group "SimGroup" when they join */
        uint32_t members[4] = {addr, addr + 1, addr + 2, addr + 3};
        group_create(&ext->group, "SimGroup", addr, members, 4, now_ms);
    }

    fprintf(stdout,
            "{\"type\":\"node_ext_initialized\",\"timestamp_us\":%llu"
            ",\"node_idx\":%d,\"addr\":\"0x%08X\""
            ",\"lat_e7\":%d,\"lon_e7\":%d}\n",
            (unsigned long long)now_us, node_idx, addr, ext->location.my_position.latitude_e7,
            ext->location.my_position.longitude_e7);
    fflush(stdout);
}

/* ─── Init relay path tracker + extended state ────────────────────────── */
void bridge_init(void) {
    relay_path_init();

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
