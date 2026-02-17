#include "../../test/stubs/esp_stubs.h"

#include "sim_event.h"
#include "sim_random.h"
#include "sim_emitter.h"
#include "sim_node.h"
#include "sim_radio.h"
#include "sim_scenario.h"
#include "sim_metrics.h"
#include "sim_anomaly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Global simulation state ──────────────────────────────────────────── */
static node_array_t g_nodes;
static radio_config_t g_radio;
static event_queue_t g_events;
static pcg32_state_t g_rng;
static uint64_t g_sim_time_us = 0;
static metrics_state_t g_metrics;

/* Per-node anomaly trackers (indexed by position in g_nodes.nodes[]) */
static node_anomaly_tracker_t g_anomaly[MAX_NODES];

/* Per-message latency tracking */
#define MAX_MSG_TRACK 256
typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint64_t sent_us;
    bool     active;
} msg_tracker_t;
static msg_tracker_t g_msg_track[MAX_MSG_TRACK];

/* ─── Bramble expects this time source ─────────────────────────────────── */
uint32_t sim_get_time_ms(void) {
    return (uint32_t)(g_sim_time_us / 1000);
}

/* ─── Pull in Bramble component implementations ─────────────────────────── */
#include "../../components/routing/routing.c"
#include "../../components/routing/discovery.c"
#include "../../components/routing/forwarding.c"
#include "../../components/packet/packet.c"

/* ─── Packet receive handlers (declared before handle_event) ────────────── */

static void handle_beacon_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                   int8_t rssi, uint64_t now_us, uint32_t now_ms)
{
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, buf, len) != ESP_OK) return;

    /* Update neighbor table (snr=0 — not modelled at this layer) */
    neighbor_update(&rx->neighbors, beacon.src_addr, rssi, 0,
                    beacon.pubkey_hash, now_ms);

    /* Install or refresh direct route to this neighbor.
     * Use the same metric scale as RREP (255 - link_penalty) so routes
     * don't get incorrectly replaced by multi-hop RREP routes. */
    route_entry_t *existing = route_lookup(&rx->routes, beacon.src_addr);
    bool new_or_broken = (!existing || existing->state == ROUTE_BROKEN);

    uint8_t penalty = compute_link_penalty(rssi, 0);
    uint8_t metric  = (penalty >= 255) ? 0 : (uint8_t)(255 - penalty);

    route_install(&rx->routes, beacon.src_addr, beacon.src_addr,
                  1, metric, ROUTE_ACTIVE, now_ms);

    if (new_or_broken) {
        emit_route_added(stdout, now_us, rx->id,
                         beacon.src_addr, beacon.src_addr, 1);

        int node_idx = (int)(rx - g_nodes.nodes);
        anomaly_check_route_flap(&g_anomaly[node_idx].flap,
                                  beacon.src_addr, beacon.src_addr,
                                  now_us, stdout, rx->id);
    }
    /* Record beacon receive for black-hole tracking */
    {
        int node_idx = (int)(rx - g_nodes.nodes);
        anomaly_record_rx(&g_anomaly[node_idx].blackhole, now_us);
    }
}

static void handle_rreq_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  int8_t rssi, uint64_t now_us, uint32_t now_ms)
{
    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, buf, len) != ESP_OK) return;

    /* Dedup: drop if we've already forwarded this query */
    if (rreq_dedup_check_and_add(&rx->rreq_dedup, rreq.query_id, now_ms)) return;

    /* Store reverse route so RREP can find its way back */
    reverse_route_add(&rx->reverse_routes, rreq.query_id, rreq.prev_hop, now_ms);

    if (rreq.header.dest_addr == rx->addr) {
        /* We are the destination — answer with RREP */
        bramble_rrep_t rrep = rrep_build_destination(&rreq, rx->addr);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rrep_serialize(&rrep, pkt.data, RREP_SIZE);
        pkt.len          = RREP_SIZE;
        pkt.is_broadcast = false;
        pkt.dest_addr    = rreq.prev_hop;   /* unicast toward originator */
        pkt.pkt_type     = PKT_TYPE_RREP;

        sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, now_us);
    } else if (rreq.header.hop_limit > 1) {
        /* Forward RREQ (snr=0 — not modelled) */
        bramble_rreq_t fwd = rreq_forward(&rreq, rx->addr, rssi, 0);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rreq_serialize(&fwd, pkt.data, RREQ_SIZE);
        pkt.len          = RREQ_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr    = 0xFFFFFFFF;
        pkt.pkt_type     = PKT_TYPE_RREQ;
        rx->packets_forwarded++;

        /* Track excessive RREQ retransmissions */
        {
            int node_idx = (int)(rx - g_nodes.nodes);
            anomaly_check_rreq_retx(&g_anomaly[node_idx].rreq_retx,
                                     rreq.header.dest_addr, now_us,
                                     stdout, rx->id);
            anomaly_record_fwd(&g_anomaly[node_idx].blackhole, now_us);
        }

        sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, now_us);
    }
    /* Always count RREQ receive for black-hole tracking */
    {
        int node_idx = (int)(rx - g_nodes.nodes);
        anomaly_record_rx(&g_anomaly[node_idx].blackhole, now_us);
    }
}

static void handle_rrep_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  uint32_t pkt_src_addr,
                                  uint64_t now_us, uint32_t now_ms)
{
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, buf, len) != ESP_OK) return;

    /*
     * Install forward route to rrep.src_addr (the destination node).
     * next_hop = pkt_src_addr: the node that sent this RREP packet to us.
     * This is the correct routing next hop toward the destination, NOT
     * rrep.next_hop which is the RREP delivery address (used to route the
     * RREP back toward the originator, not to route data toward the dest).
     */
    route_install(&rx->routes, rrep.src_addr, pkt_src_addr,
                  rrep.hop_count, rrep.route_metric, ROUTE_ACTIVE, now_ms);
    emit_route_added(stdout, now_us, rx->id,
                     rrep.src_addr, pkt_src_addr, rrep.hop_count);

    int node_idx = (int)(rx - g_nodes.nodes);
    anomaly_check_route_flap(&g_anomaly[node_idx].flap,
                              rrep.src_addr, pkt_src_addr,
                              now_us, stdout, rx->id);
    anomaly_record_rx(&g_anomaly[node_idx].blackhole, now_us);

    /* Are we the original RREQ sender? */
    pending_discovery_t *pd =
        discovery_lookup_by_query(&rx->pending_discoveries, rrep.query_id);
    if (pd) {
        /* Route acquired — remove pending discovery */
        discovery_remove(&rx->pending_discoveries, pd->dest_addr);
        /* Data delivery happens when the rescheduled EVT_GENERATE_MESSAGE fires */
        return;
    }

    /* Not the originator — forward RREP back toward originator */
    reverse_route_t *rr =
        reverse_route_lookup(&rx->reverse_routes, rrep.query_id);
    if (!rr) return;  /* reverse route expired — drop */

    bramble_rrep_t fwd = rrep_forward(&rrep, rr->prev_hop);

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    bramble_rrep_serialize(&fwd, pkt.data, RREP_SIZE);
    pkt.len          = RREP_SIZE;
    pkt.is_broadcast = false;
    pkt.dest_addr    = rr->prev_hop;
    pkt.pkt_type     = PKT_TYPE_RREP;
    rx->packets_forwarded++;

    sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                        &g_events, &g_metrics, now_us);
}

static void handle_rerr_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  uint64_t now_us, uint32_t now_ms)
{
    (void)now_ms;
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, buf, len) != ESP_OK) return;

    rerr_handle(&rx->routes, &rerr);
    emit_link_broken(stdout, now_us, rx->id, rerr.broken_next_hop);
}

static void handle_data_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  uint64_t now_us, uint32_t now_ms)
{
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) return;

    int node_idx = (int)(rx - g_nodes.nodes);

    /* Track DATA receive for black-hole detection */
    anomaly_record_rx(&g_anomaly[node_idx].blackhole, now_us);

    /* Route-loop detection: check if this node already forwarded this packet */
    anomaly_check_loop(&g_anomaly[node_idx].loop,
                       hdr.packet_id, now_us, stdout, rx->id);

    if (hdr.dest_addr == rx->addr) {
        /* Final destination — record delivery */
        for (int i = 0; i < MAX_MSG_TRACK; i++) {
            if (g_msg_track[i].active &&
                g_msg_track[i].packet_id == hdr.packet_id) {
                uint64_t latency_us = now_us - g_msg_track[i].sent_us;
                metrics_record_packet_delivered(&g_metrics, latency_us);
                g_msg_track[i].active = false;
                break;
            }
        }
        fprintf(stdout,
            "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"packet_id\":\"0x%08X\"}\n",
            (unsigned long long)now_us, rx->id, hdr.packet_id);
        fflush(stdout);
        /* Delivery counts as a forward for black-hole tracking */
        anomaly_record_fwd(&g_anomaly[node_idx].blackhole, now_us);
        return;
    }

    /* Not final destination — forward */
    uint8_t hop_limit = hdr.hop_limit;
    forward_result_t fwd_res =
        forward_data(&rx->routes, hdr.dest_addr, &hop_limit, now_ms);

    if (fwd_res.route_error) {
        /* Build and broadcast RERR */
        bramble_rerr_t rerr = rerr_build(rx->addr, hdr.dest_addr, fwd_res.next_hop);

        outbound_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        bramble_rerr_serialize(&rerr, pkt.data, RERR_SIZE);
        pkt.len          = RERR_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr    = 0xFFFFFFFF;
        pkt.pkt_type     = PKT_TYPE_RERR;

        sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, now_us);
        emit_packet_dropped(stdout, now_us, rx->id, "no_route");
        return;
    }

    if (!fwd_res.should_send) return;

    /* Patch hop_limit in buffer (byte 3 of header) and forward */
    uint8_t fwd_buf[256];
    memcpy(fwd_buf, buf, len);
    fwd_buf[3] = hop_limit;

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, fwd_buf, len);
    pkt.len          = len;
    pkt.is_broadcast = false;
    pkt.dest_addr    = fwd_res.next_hop;
    pkt.pkt_type     = PKT_TYPE_DATA;
    rx->packets_forwarded++;

    /* Count forwarded data for black-hole tracking */
    anomaly_record_fwd(&g_anomaly[node_idx].blackhole, now_us);

    sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                        &g_events, &g_metrics, now_us);
}

/* ─── Main event dispatcher ─────────────────────────────────────────────── */

static void handle_event(sim_event_t *event) {
    switch (event->type) {

        case EVT_NODE_MOVE: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (node) {
                node_move(node, event->data.node.x, event->data.node.y);
                emit_node_moved(stdout, event->timestamp_us, node->id, node->x, node->y);
            }
            break;
        }

        case EVT_NODE_LEAVE: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (node) {
                node_deactivate(node);
                emit_node_left(stdout, event->timestamp_us, node->id);
                /* Check for mesh partition after topology change */
                anomaly_check_partition(&g_nodes, g_radio.range,
                                        event->timestamp_us, stdout);
            }
            break;
        }

        case EVT_NODE_JOIN: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (node) {
                node_activate(node);
                emit_node_joined(stdout, event->timestamp_us, node->id, node->addr, node->x, node->y);
            }
            break;
        }

        case EVT_INTERFERENCE_START: {
            int zone_idx = radio_add_interference_zone(&g_radio,
                event->data.interference.center_x,
                event->data.interference.center_y,
                event->data.interference.radius);
            fprintf(stderr, "Interference zone %d started at (%.1f, %.1f) radius %.1f\n",
                    zone_idx, event->data.interference.center_x,
                    event->data.interference.center_y, event->data.interference.radius);
            break;
        }

        case EVT_INTERFERENCE_END: {
            if (event->data.interference.zone_index >= 0) {
                radio_clear_interference_zone(&g_radio, event->data.interference.zone_index);
                fprintf(stderr, "Interference zone %d ended\n", event->data.interference.zone_index);
            }
            break;
        }

        case EVT_METRICS_TICK: {
            int active = 0;
            for (int i = 0; i < g_nodes.count; i++) {
                if (g_nodes.nodes[i].active) active++;
            }
            metrics_update_active_nodes(&g_metrics, active);
            emit_metrics(stdout, event->timestamp_us, active,
                         g_metrics.total_packets, g_metrics.messages_sent,
                         g_metrics.delivered_packets,
                         g_metrics.dropped_packets, metrics_avg_latency_ms(&g_metrics));

            /* Check black-hole anomaly on each active node at every metrics tick */
            for (int i = 0; i < g_nodes.count; i++) {
                if (g_nodes.nodes[i].active) {
                    anomaly_check_blackhole(&g_anomaly[i].blackhole,
                                            event->timestamp_us,
                                            stdout, g_nodes.nodes[i].id);
                }
            }
            break;
        }

        case EVT_TICK_NODE: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.tick.node_id);
            if (!node || !node->active) break;

            node_tick_result_t tick_result;
            node_tick(node, event->timestamp_us, &tick_result);

            /* Transmit all outbound packets via radio */
            for (int i = 0; i < tick_result.count; i++) {
                sim_radio_broadcast(node, &tick_result.pkts[i],
                                    &g_nodes, &g_radio, &g_rng,
                                    &g_events, &g_metrics, event->timestamp_us);
            }

            /* Reschedule next tick */
            sim_event_t next;
            memset(&next, 0, sizeof(next));
            next.type                = EVT_TICK_NODE;
            next.timestamp_us        = event->timestamp_us + NODE_TICK_INTERVAL_US;
            memcpy(next.data.tick.node_id, node->id, NODE_ID_LEN);
            next.data.tick.tick_seq  = event->data.tick.tick_seq + 1;
            event_queue_push(&g_events, &next);
            break;
        }

        case EVT_RECEIVE_PACKET: {
            /* dest_addr is the specific rx node (broadcast → each rx gets its own event) */
            sim_node_t *rx = node_array_find_by_addr(&g_nodes,
                                                     event->data.packet.dest_addr);
            if (!rx || !rx->active) break;

            uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);
            const uint8_t *buf = event->data.packet.data;
            uint16_t len       = event->data.packet.len;
            int8_t   rssi      = event->data.packet.rssi;

            /* Deserialize header to get packet type */
            bramble_header_t hdr;
            if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) break;

            emit_packet_received_typed(stdout, event->timestamp_us, rx->id,
                                       event->data.packet.src_addr,
                                       rssi, len, hdr.type);
            rx->packets_received++;

            switch (hdr.type) {
                case PKT_TYPE_BEACON:
                    handle_beacon_received(rx, buf, len, rssi,
                                           event->timestamp_us, now_ms);
                    break;
                case PKT_TYPE_RREQ:
                    handle_rreq_received(rx, buf, len, rssi,
                                         event->timestamp_us, now_ms);
                    break;
                case PKT_TYPE_RREP:
                    handle_rrep_received(rx, buf, len,
                                         event->data.packet.src_addr,
                                         event->timestamp_us, now_ms);
                    break;
                case PKT_TYPE_RERR:
                    handle_rerr_received(rx, buf, len,
                                         event->timestamp_us, now_ms);
                    break;
                case PKT_TYPE_DATA:
                    handle_data_received(rx, buf, len,
                                         event->timestamp_us, now_ms);
                    break;
                default:
                    break;
            }
            break;
        }

        case EVT_GENERATE_MESSAGE: {
            sim_node_t *src =
                node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (!src || !src->active) break;

            uint32_t dest_addr = event->data.node.addr;
            uint32_t now_ms    = (uint32_t)(event->timestamp_us / 1000);

            /* Look up route to destination */
            route_entry_t *route = route_lookup(&src->routes, dest_addr);

            if (!route || route->state == ROUTE_BROKEN ||
                          route->state == ROUTE_DISCOVERING) {
                /* Start route discovery if not already pending */
                pending_discovery_t *pd =
                    discovery_lookup(&src->pending_discoveries, dest_addr);
                if (!pd) {
                    uint32_t query_id = pcg32_random(&g_rng);
                    discovery_start(&src->pending_discoveries,
                                    dest_addr, query_id, now_ms);

                    bramble_rreq_t rreq =
                        rreq_build_originator(src->addr, dest_addr,
                                              query_id, src->addr);
                    /* Simulator override: increase hop limit for large meshes */
                    rreq.header.hop_limit = 32;

                    outbound_packet_t pkt;
                    memset(&pkt, 0, sizeof(pkt));
                    bramble_rreq_serialize(&rreq, pkt.data, RREQ_SIZE);
                    pkt.len          = RREQ_SIZE;
                    pkt.is_broadcast = true;
                    pkt.dest_addr    = 0xFFFFFFFF;
                    pkt.pkt_type     = PKT_TYPE_RREQ;

                    sim_radio_broadcast(src, &pkt, &g_nodes, &g_radio, &g_rng,
                                        &g_events, &g_metrics, event->timestamp_us);
                }

                /* Reschedule message generation after discovery window (200 ms) */
                sim_event_t retry = *event;
                retry.timestamp_us += 200000ULL;  /* 200 ms */
                event_queue_push(&g_events, &retry);
                break;
            }

            /* Route exists — build and send DATA packet */
            uint8_t hop_limit = 32;  /* Simulator override for large meshes */
            forward_result_t fwd_res =
                forward_data(&src->routes, dest_addr, &hop_limit, now_ms);
            if (!fwd_res.should_send) break;

            bramble_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.version   = BRAMBLE_VERSION;
            hdr.type      = PKT_TYPE_DATA;
            hdr.flags     = 0;
            hdr.hop_limit = hop_limit;
            hdr.dest_addr = dest_addr;
            hdr.packet_id = pcg32_random(&g_rng);

            uint8_t data_buf[HEADER_SIZE];
            bramble_header_serialize(&hdr, data_buf, HEADER_SIZE);

            /* Track for latency measurement */
            for (int i = 0; i < MAX_MSG_TRACK; i++) {
                if (!g_msg_track[i].active) {
                    g_msg_track[i].active    = true;
                    g_msg_track[i].packet_id = hdr.packet_id;
                    g_msg_track[i].dest_addr = dest_addr;
                    g_msg_track[i].sent_us   = event->timestamp_us;
                    break;
                }
            }

            outbound_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            memcpy(pkt.data, data_buf, HEADER_SIZE);
            pkt.len          = HEADER_SIZE;
            pkt.is_broadcast = false;
            pkt.dest_addr    = fwd_res.next_hop;
            pkt.pkt_type     = PKT_TYPE_DATA;
            src->packets_originated++;

            sim_radio_broadcast(src, &pkt, &g_nodes, &g_radio, &g_rng,
                                &g_events, &g_metrics, event->timestamp_us);

            metrics_record_message_sent(&g_metrics);
            fprintf(stdout,
                "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
                ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"packet_id\":\"0x%08X\"}\n",
                (unsigned long long)event->timestamp_us,
                src->id, dest_addr, hdr.packet_id);
            fflush(stdout);
            break;
        }

        default:
            fprintf(stderr, "Unhandled event type %d\n", event->type);
            break;
    }
}

/* ─── Entry point ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <scenario.json>\n", argv[0]);
        return 1;
    }

    /* Initialize simulation state */
    node_array_init(&g_nodes);
    event_queue_init(&g_events);
    metrics_init(&g_metrics);
    memset(g_msg_track, 0, sizeof(g_msg_track));
    for (int i = 0; i < MAX_NODES; i++) {
        anomaly_init(&g_anomaly[i]);
    }

    /* Load scenario */
    scenario_t scenario = {
        .nodes  = &g_nodes,
        .radio  = &g_radio,
        .events = &g_events,
        .rng    = &g_rng,
    };

    if (!scenario_load_file(argv[1], &scenario)) {
        fprintf(stderr, "Error: failed to load scenario\n");
        return 1;
    }

    pcg32_seed(&g_rng, scenario.metadata.seed);

    fprintf(stderr, "Loaded scenario '%s'\n", scenario.metadata.name);
    fprintf(stderr, "  Duration: %llu ms\n",
            (unsigned long long)(scenario.metadata.duration_us / 1000));
    fprintf(stderr, "  Nodes: %d\n", g_nodes.count);
    fprintf(stderr, "  Events: %d\n", event_queue_count(&g_events));
    fprintf(stderr, "  Mode: %s\n",
            scenario.metadata.deterministic ? "deterministic" : "stochastic");
    fprintf(stderr, "  Seed: %llu\n",
            (unsigned long long)scenario.metadata.seed);
    fprintf(stderr, "\nStarting simulation...\n\n");

    /* Emit initial node positions */
    for (int i = 0; i < g_nodes.count; i++) {
        sim_node_t *node = &g_nodes.nodes[i];
        if (node->active) {
            emit_node_joined(stdout, 0, node->id, node->addr, node->x, node->y);
        }
    }

    /* Emit initial radio config for UI */
    fprintf(stdout,
            "{\"type\":\"config\",\"timestamp_us\":0,\"radio_range\":%.2f}\n",
            g_radio.range);
    fflush(stdout);

    /* Schedule initial node ticks (staggered by 100 ms per node) */
    for (int i = 0; i < g_nodes.count; i++) {
        sim_node_t *node = &g_nodes.nodes[i];
        if (!node->active) continue;

        sim_event_t tick_evt;
        memset(&tick_evt, 0, sizeof(tick_evt));
        tick_evt.type         = EVT_TICK_NODE;
        tick_evt.timestamp_us = (uint64_t)i * 100000ULL;  /* 100 ms stagger */
        strncpy(tick_evt.data.tick.node_id, node->id, NODE_ID_LEN - 1);
        tick_evt.data.tick.tick_seq = 0;
        event_queue_push(&g_events, &tick_evt);
    }

    /* Main event loop */
    sim_event_t event;
    while (event_queue_pop(&g_events, &event)) {
        if (event.timestamp_us > scenario.metadata.duration_us)
            break;

        g_sim_time_us = event.timestamp_us;
        handle_event(&event);
    }

    /* Emit final metrics to stdout so the UI gets the final counts */
    {
        int active = 0;
        for (int i = 0; i < g_nodes.count; i++) {
            if (g_nodes.nodes[i].active) active++;
        }
        emit_metrics(stdout, g_sim_time_us, active,
                     g_metrics.total_packets, g_metrics.messages_sent,
                     g_metrics.delivered_packets,
                     g_metrics.dropped_packets, metrics_avg_latency_ms(&g_metrics));
        fflush(stdout);
    }

    fprintf(stderr, "\nSimulation complete.\n");
    fprintf(stderr, "Final metrics:\n");
    fprintf(stderr, "  Total packets: %llu\n",
            (unsigned long long)g_metrics.total_packets);
    fprintf(stderr, "  Delivered: %llu\n",
            (unsigned long long)g_metrics.delivered_packets);
    fprintf(stderr, "  Dropped: %llu\n",
            (unsigned long long)g_metrics.dropped_packets);
    fprintf(stderr, "  Delivery rate: %.2f%%\n",
            metrics_delivery_rate(&g_metrics) * 100.0);
    fprintf(stderr, "  Avg latency: %.3f ms\n",
            metrics_avg_latency_ms(&g_metrics));

    return 0;
}
