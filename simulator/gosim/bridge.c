#include "bridge.h"
#include "../../components/packet/include/packet.h"
#include "../../components/routing/include/routing.h"
#include "../../components/routing/include/discovery.h"
#include "../../components/routing/include/forwarding.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ─── Global simulation time ───────────────────────────────────────────── */
uint64_t g_bridge_sim_time_us = 0;

void bridge_set_sim_time(uint64_t us) {
    g_bridge_sim_time_us = us;
}

uint32_t sim_get_time_ms(void) {
    return (uint32_t)(g_bridge_sim_time_us / 1000);
}

/* ─── Event union accessors ────────────────────────────────────────────── */

node_event_data_t bridge_get_node_event(const sim_event_t *e) {
    return e->data.node;
}

packet_event_data_t bridge_get_packet_event(const sim_event_t *e) {
    return e->data.packet;
}

tick_event_data_t bridge_get_tick_event(const sim_event_t *e) {
    return e->data.tick;
}

interference_event_data_t bridge_get_interference_event(const sim_event_t *e) {
    return e->data.interference;
}

event_type_t bridge_get_event_type(const sim_event_t *e) {
    return e->type;
}

uint64_t bridge_get_event_timestamp(const sim_event_t *e) {
    return e->timestamp_us;
}

/* ─── Event construction helpers ───────────────────────────────────────── */

sim_event_t bridge_make_tick_event(uint64_t ts_us, const char *node_id, uint32_t tick_seq) {
    sim_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_TICK_NODE;
    e.timestamp_us = ts_us;
    strncpy(e.data.tick.node_id, node_id, NODE_ID_LEN - 1);
    e.data.tick.tick_seq = tick_seq;
    return e;
}

sim_event_t bridge_make_node_event(event_type_t type, uint64_t ts_us,
                                    const char *node_id, uint32_t addr, float x, float y) {
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

sim_event_t bridge_make_generate_msg_event(uint64_t ts_us, const char *node_id, uint32_t dest_addr) {
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

void bridge_msg_track_init(msg_tracker_t *track, int count) {
    memset(track, 0, sizeof(msg_tracker_t) * count);
}

int bridge_msg_track_add(msg_tracker_t *track, int count,
                          uint32_t packet_id, uint32_t src_addr, uint32_t dest_addr, uint64_t sent_us) {
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

uint32_t bridge_msg_track_find_src(msg_tracker_t *track, int count, uint32_t packet_id) {
    for (int i = 0; i < count; i++) {
        if (track[i].active && track[i].packet_id == packet_id) {
            return track[i].src_addr;
        }
    }
    return 0;
}

bool bridge_msg_track_complete(msg_tracker_t *track, int count,
                                uint32_t packet_id, uint64_t now_us,
                                metrics_state_t *metrics) {
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

static void _handle_beacon(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                            int8_t rssi, uint64_t now_us, uint32_t now_ms,
                            node_array_t *nodes, node_anomaly_tracker_t *anomaly) {
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, buf, len) != ESP_OK) return;

    neighbor_update(&rx->neighbors, beacon.src_addr, rssi, 0,
                    beacon.pubkey_hash, now_ms);

    route_entry_t *existing = route_lookup(&rx->routes, beacon.src_addr);
    bool new_or_broken = (!existing || existing->state == ROUTE_BROKEN);

    uint8_t penalty = compute_link_penalty(rssi, 0);
    uint8_t metric = (penalty >= 255) ? 0 : (uint8_t)(255 - penalty);

    route_install(&rx->routes, beacon.src_addr, beacon.src_addr,
                  1, metric, ROUTE_ACTIVE, now_ms);

    if (new_or_broken) {
        emit_route_added(stdout, now_us, rx->id,
                         beacon.src_addr, beacon.src_addr, 1);

        int node_idx = (int)(rx - nodes->nodes);
        anomaly_check_route_flap(&anomaly[node_idx].flap,
                                  beacon.src_addr, beacon.src_addr,
                                  now_us, stdout, rx->id);
    }
}

static void _handle_rreq(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                           int8_t rssi, uint64_t now_us, uint32_t now_ms,
                           node_array_t *nodes, radio_config_t *radio,
                           pcg32_state_t *rng, event_queue_t *events,
                           metrics_state_t *metrics, node_anomaly_tracker_t *anomaly) {
    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, buf, len) != ESP_OK) return;

    if (rreq_dedup_check_and_add(&rx->rreq_dedup, rreq.query_id, now_ms)) return;

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
            anomaly_check_rreq_retx(&anomaly[node_idx].rreq_retx,
                                     rreq.header.dest_addr, now_us,
                                     stdout, rx->id);
        }

        sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
    }
}

static void _handle_rrep(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                           uint32_t pkt_src_addr, uint64_t now_us, uint32_t now_ms,
                           node_array_t *nodes, radio_config_t *radio,
                           pcg32_state_t *rng, event_queue_t *events,
                           metrics_state_t *metrics, node_anomaly_tracker_t *anomaly) {
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, buf, len) != ESP_OK) return;

    route_install(&rx->routes, rrep.src_addr, pkt_src_addr,
                  rrep.hop_count, rrep.route_metric, ROUTE_ACTIVE, now_ms);
    emit_route_added(stdout, now_us, rx->id,
                     rrep.src_addr, pkt_src_addr, rrep.hop_count);

    int node_idx = (int)(rx - nodes->nodes);
    anomaly_check_route_flap(&anomaly[node_idx].flap,
                              rrep.src_addr, pkt_src_addr,
                              now_us, stdout, rx->id);

    pending_discovery_t *pd =
        discovery_lookup_by_query(&rx->pending_discoveries, rrep.query_id);
    if (pd) {
        discovery_remove(&rx->pending_discoveries, pd->dest_addr);
        return;
    }

    reverse_route_t *rr =
        reverse_route_lookup(&rx->reverse_routes, rrep.query_id);
    if (!rr) return;

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

static void _handle_rerr(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                           uint64_t now_us, uint32_t now_ms) {
    (void)now_ms;
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, buf, len) != ESP_OK) return;

    rerr_handle(&rx->routes, &rerr);
    emit_link_broken(stdout, now_us, rx->id, rerr.broken_next_hop);
}

static void _handle_delivery_receipt(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                      uint64_t now_us, uint32_t now_ms,
                                      node_array_t *nodes, radio_config_t *radio,
                                      pcg32_state_t *rng, event_queue_t *events,
                                      metrics_state_t *metrics, node_anomaly_tracker_t *anomaly,
                                      msg_tracker_t *msg_track, int msg_track_count) {
    bramble_delivery_receipt_t receipt;
    if (bramble_delivery_receipt_deserialize(&receipt, buf, len) != ESP_OK) return;

    if (receipt.header.dest_addr == rx->addr) {
        /* This receipt is for us — the original sender */
        bool was_tracked = bridge_msg_track_complete(msg_track, msg_track_count,
                                                      receipt.orig_packet_id, now_us, metrics);
        if (was_tracked) {
            /* Check if this was a retried message */
            pending_ack_t *pa = NULL;
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
            pending_ack_remove(&rx->pending_acks, receipt.orig_packet_id);
            flow_on_ack(&rx->flow_control, receipt.src_addr);
        }

        /* Emit delivered with path */
        fprintf(stdout,
            "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"packet_id\":\"0x%08X\""
            ",\"from\":\"0x%08X\",\"hops\":%d,\"path\":[",
            (unsigned long long)now_us, rx->id, receipt.orig_packet_id,
            receipt.src_addr, receipt.hop_count);
        for (int i = 0; i < receipt.hop_count && i < DELIVERY_RECEIPT_MAX_HOPS; i++) {
            if (i > 0) fprintf(stdout, ",");
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
    if (!fwd_res.should_send) return;

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

static void _handle_data(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                           uint64_t now_us, uint32_t now_ms,
                           node_array_t *nodes, radio_config_t *radio,
                           pcg32_state_t *rng, event_queue_t *events,
                           metrics_state_t *metrics, node_anomaly_tracker_t *anomaly,
                           msg_tracker_t *msg_track, int msg_track_count) {
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) return;

    int node_idx = (int)(rx - nodes->nodes);

    anomaly_record_rx(&anomaly[node_idx].blackhole, now_us);
    anomaly_check_loop(&anomaly[node_idx].loop,
                       hdr.packet_id, now_us, stdout, rx->id);

    if (hdr.dest_addr == rx->addr) {
        /* Message reached destination — send delivery receipt back to source */
        uint32_t orig_sender = bridge_msg_track_find_src(msg_track, msg_track_count,
                                                          hdr.packet_id);

        /* Record delivery immediately (don't wait for receipt to arrive at source) */
        bridge_msg_track_complete(msg_track, msg_track_count,
                                   hdr.packet_id, now_us, metrics);

        anomaly_record_fwd(&anomaly[node_idx].blackhole, now_us);

        if (orig_sender != 0) {
            /* Build and send delivery receipt */
            bramble_delivery_receipt_t receipt;
            memset(&receipt, 0, sizeof(receipt));
            receipt.header.version = BRAMBLE_VERSION;
            receipt.header.type = PKT_TYPE_DELIVERY_RECEIPT;
            receipt.header.flags = 0;
            receipt.header.hop_limit = 32;
            receipt.header.dest_addr = orig_sender;
            receipt.header.packet_id = pcg32_random(rng);
            receipt.src_addr = rx->addr;
            receipt.orig_packet_id = hdr.packet_id;
            receipt.hop_count = 0; /* TODO: accumulate relay path in forwarding */

            uint8_t receipt_buf[DELIVERY_RECEIPT_MAX_SIZE];
            if (bramble_delivery_receipt_serialize(&receipt, receipt_buf, sizeof(receipt_buf)) == ESP_OK) {
                /* Route receipt back to sender */
                uint8_t rcpt_hop = 32;
                forward_result_t fwd_res =
                    forward_data(&rx->routes, orig_sender, &rcpt_hop, now_ms);
                if (fwd_res.should_send) {
                    outbound_packet_t pkt;
                    memset(&pkt, 0, sizeof(pkt));
                    memcpy(pkt.data, receipt_buf, DELIVERY_RECEIPT_MIN_SIZE);
                    pkt.len = DELIVERY_RECEIPT_MIN_SIZE;
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
    forward_result_t fwd_res =
        forward_data(&rx->routes, hdr.dest_addr, &hop_limit, now_ms);

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
        return;
    }

    if (!fwd_res.should_send) return;

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

void bridge_handle_receive_packet(
    sim_event_t *event,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    msg_tracker_t *msg_track,
    int msg_track_count)
{
    sim_node_t *rx = node_array_find_by_addr(nodes, event->data.packet.dest_addr);
    if (!rx || !rx->active) return;

    uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);
    const uint8_t *buf = event->data.packet.data;
    uint16_t len = event->data.packet.len;
    int8_t rssi = event->data.packet.rssi;

    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) return;

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

    emit_packet_received_typed(stdout, event->timestamp_us, rx->id,
                               event->data.packet.src_addr,
                               rssi, len, hdr.type);
    rx->packets_received++;

    switch (hdr.type) {
        case PKT_TYPE_BEACON:
            _handle_beacon(rx, buf, len, rssi, event->timestamp_us, now_ms,
                           nodes, anomaly);
            break;
        case PKT_TYPE_RREQ:
            _handle_rreq(rx, buf, len, rssi, event->timestamp_us, now_ms,
                         nodes, radio, rng, events, metrics, anomaly);
            break;
        case PKT_TYPE_RREP:
            _handle_rrep(rx, buf, len, event->data.packet.src_addr,
                         event->timestamp_us, now_ms,
                         nodes, radio, rng, events, metrics, anomaly);
            break;
        case PKT_TYPE_RERR:
            _handle_rerr(rx, buf, len, event->timestamp_us, now_ms);
            break;
        case PKT_TYPE_DATA:
            _handle_data(rx, buf, len, event->timestamp_us, now_ms,
                         nodes, radio, rng, events, metrics, anomaly,
                         msg_track, msg_track_count);
            break;
        case PKT_TYPE_DELIVERY_RECEIPT:
            _handle_delivery_receipt(rx, buf, len, event->timestamp_us, now_ms,
                                     nodes, radio, rng, events, metrics, anomaly,
                                     msg_track, msg_track_count);
            break;
        default:
            break;
    }
}

void bridge_handle_generate_message(
    sim_event_t *event,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    msg_tracker_t *msg_track,
    int msg_track_count)
{
    sim_node_t *src = node_array_find_by_id(nodes, event->data.node.node_id);
    if (!src || !src->active) return;

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
            (unsigned long long)event->timestamp_us,
            src->id, dest_addr, retry_count);
        fflush(stdout);
        return;
    }

    route_entry_t *route = route_lookup(&src->routes, dest_addr);

    if (!route || route->state == ROUTE_BROKEN ||
                  route->state == ROUTE_DISCOVERING) {
        pending_discovery_t *pd =
            discovery_lookup(&src->pending_discoveries, dest_addr);
        if (pd && (now_ms - pd->timestamp > 5000)) {
            discovery_remove(&src->pending_discoveries, dest_addr);
            pd = NULL;
        }
        bool should_send_rreq = false;
        if (!pd) {
            uint32_t query_id = pcg32_random(rng);
            discovery_start(&src->pending_discoveries, dest_addr, query_id, now_ms);
            should_send_rreq = true;
        } else if (pd->attempts < MAX_RREQ_ATTEMPTS &&
                   (now_ms - pd->timestamp) > 1000) {
            pd->attempts++;
            pd->query_id = pcg32_random(rng);
            pd->timestamp = now_ms;
            should_send_rreq = true;
        }

        if (should_send_rreq) {
            pd = discovery_lookup(&src->pending_discoveries, dest_addr);
            bramble_rreq_t rreq =
                rreq_build_originator(src->addr, dest_addr,
                                      pd->query_id, src->addr);
            rreq.header.hop_limit = 32;

            outbound_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            bramble_rreq_serialize(&rreq, pkt.data, RREQ_SIZE);
            pkt.len = RREQ_SIZE;
            pkt.is_broadcast = true;
            pkt.dest_addr = 0xFFFFFFFF;
            pkt.pkt_type = PKT_TYPE_RREQ;

            sim_radio_broadcast(src, &pkt, nodes, radio, rng,
                                events, metrics, event->timestamp_us);

            {
                int src_idx = (int)(src - nodes->nodes);
                anomaly_check_rreq_retx(&anomaly[src_idx].rreq_retx,
                                         dest_addr, event->timestamp_us,
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
    uint8_t hop_limit = 32;
    forward_result_t fwd_res =
        forward_data(&src->routes, dest_addr, &hop_limit, now_ms);
    if (!fwd_res.should_send) return;

    bramble_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = BRAMBLE_VERSION;
    hdr.type = PKT_TYPE_DATA;
    hdr.flags = 0;
    hdr.hop_limit = hop_limit;
    hdr.dest_addr = dest_addr;
    hdr.packet_id = pcg32_random(rng);

    uint8_t data_buf[HEADER_SIZE];
    bramble_header_serialize(&hdr, data_buf, HEADER_SIZE);

    bridge_msg_track_add(msg_track, msg_track_count,
                          hdr.packet_id, src->addr, dest_addr, event->timestamp_us);

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, data_buf, HEADER_SIZE);
    pkt.len = HEADER_SIZE;
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;
    src->packets_originated++;

    sim_radio_broadcast(src, &pkt, nodes, radio, rng,
                        events, metrics, event->timestamp_us);

    metrics_record_message_sent(metrics);
    fprintf(stdout,
        "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"packet_id\":\"0x%08X\"}\n",
        (unsigned long long)event->timestamp_us,
        src->id, dest_addr, hdr.packet_id);
    fflush(stdout);
}
