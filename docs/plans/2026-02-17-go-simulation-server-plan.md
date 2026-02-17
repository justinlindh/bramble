# Go Simulation Server Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Replace the C `main.c` + Node.js relay with a single Go server that embeds the C simulation modules via cgo, providing real-time interactive simulation with WebSocket streaming.

**Architecture:** Go binary compiles all C sim modules + Bramble components via cgo single-compilation-unit. Go manages the sim event loop, WebSocket connections, HTTP static file serving, and command processing. React UI unchanged.

**Tech Stack:** Go 1.25, cgo, gorilla/websocket, existing C sim modules, React/TypeScript/Vite (unchanged)

**Design doc:** `docs/plans/2026-02-17-go-simulation-server-design.md`

---

## Phase 1: C Bridge Foundation

### Task 1: Create Go module and directory structure

**Files:**
- Create: `simulator/gosim/go.mod`
- Create: `simulator/gosim/main.go` (placeholder)

**Step 1: Create the Go module**

```bash
cd simulator/gosim
go mod init bramble-sim
```

**Step 2: Create placeholder main.go**

```go
// simulator/gosim/main.go
package main

import "fmt"

func main() {
	fmt.Println("bramble-gosim placeholder")
}
```

**Step 3: Verify it compiles**

Run: `cd simulator/gosim && go build -o bramble-gosim .`
Expected: Binary created, runs and prints placeholder message.

**Step 4: Commit**

```bash
git add simulator/gosim/
git commit -m "feat(sim): init Go module for simulation server"
```

---

### Task 2: Create C single-compilation-unit and bridge header

**Files:**
- Create: `simulator/gosim/csrc/all.c`
- Create: `simulator/gosim/csrc/bridge.h`
- Create: `simulator/gosim/csrc/bridge.c`

The `all.c` file includes all C sources so cgo compiles them as one unit. The bridge provides helper functions for accessing C unions and wrapping complex event dispatch logic.

**Step 1: Create `all.c`**

```c
// simulator/gosim/csrc/all.c
//
// Single compilation unit — includes all C sources for cgo.
// This file is compiled by cgo as part of the Go build.
//

// ESP stubs (must come first)
#include "../../../test/stubs/esp_stubs.h"

// Simulator modules
#include "../../engine/sim_event.c"
#include "../../engine/sim_random.c"
#include "../../engine/sim_emitter.c"
#include "../../engine/sim_node.c"
#include "../../engine/sim_radio.c"
#include "../../engine/sim_scenario.c"
#include "../../engine/sim_metrics.c"
#include "../../engine/sim_anomaly.c"
#include "../../engine/cJSON.c"

// Bramble components
#include "../../../components/routing/routing.c"
#include "../../../components/routing/discovery.c"
#include "../../../components/routing/forwarding.c"
#include "../../../components/packet/packet.c"

// Bridge helpers
#include "bridge.c"
```

**Step 2: Create `bridge.h`**

```c
// simulator/gosim/csrc/bridge.h
#ifndef BRIDGE_H
#define BRIDGE_H

#include "../../engine/sim_event.h"
#include "../../engine/sim_node.h"
#include "../../engine/sim_radio.h"
#include "../../engine/sim_random.h"
#include "../../engine/sim_metrics.h"
#include "../../engine/sim_anomaly.h"
#include "../../engine/sim_scenario.h"
#include "../../engine/sim_emitter.h"
#include "../../../components/packet/include/packet.h"
#include "../../../components/routing/include/routing.h"
#include "../../../components/routing/include/discovery.h"

#include <stdio.h>
#include <stdint.h>

// ── Sim time source (Bramble's esp_timer uses this) ──────────────────
// Defined in the Go bridge; called by sim_get_time_ms() stub
extern uint64_t g_bridge_sim_time_us;

uint32_t sim_get_time_ms(void);

// ── Event union accessors ────────────────────────────────────────────
// cgo cannot access C union members directly, so we provide accessors.

void bridge_get_node_event(const sim_event_t *evt,
    char *node_id_out, uint32_t *addr_out, float *x_out, float *y_out);

void bridge_get_packet_event(const sim_event_t *evt,
    uint32_t *src_out, uint32_t *dest_out, int8_t *rssi_out,
    uint8_t *data_out, uint16_t *len_out);

void bridge_get_tick_event(const sim_event_t *evt,
    char *node_id_out, uint32_t *tick_seq_out);

void bridge_get_interference_event(const sim_event_t *evt,
    int *zone_idx_out, float *cx_out, float *cy_out, float *radius_out);

event_type_t bridge_get_event_type(const sim_event_t *evt);
uint64_t bridge_get_event_timestamp(const sim_event_t *evt);

// ── Event construction helpers ───────────────────────────────────────
void bridge_make_tick_event(sim_event_t *evt, uint64_t timestamp_us,
    const char *node_id, uint32_t tick_seq);

void bridge_make_node_event(sim_event_t *evt, uint64_t timestamp_us,
    event_type_t type, const char *node_id, uint32_t addr, float x, float y);

void bridge_make_generate_msg_event(sim_event_t *evt, uint64_t timestamp_us,
    const char *src_id, uint32_t dest_addr);

void bridge_make_interference_start(sim_event_t *evt, uint64_t timestamp_us,
    float cx, float cy, float radius);

void bridge_make_interference_end(sim_event_t *evt, uint64_t timestamp_us,
    int zone_index);

// ── Packet handling (wraps protocol logic from main.c) ───────────────
// Each returns outbound packets via the event queue + emits JSON to `out`.

void bridge_handle_receive_packet(
    sim_event_t *evt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    FILE *out);

void bridge_handle_generate_message(
    sim_event_t *evt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    FILE *out);

// ── Message tracking (for delivery latency) ──────────────────────────
#define MAX_MSG_TRACK 1024

typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint64_t sent_us;
    int      active;  // bool as int for cgo
} msg_tracker_t;

void bridge_msg_track_init(msg_tracker_t *track, int count);
void bridge_msg_track_add(msg_tracker_t *track, int count,
    uint32_t packet_id, uint32_t dest_addr, uint64_t sent_us);
int bridge_msg_track_complete(msg_tracker_t *track, int count,
    uint32_t packet_id, uint64_t now_us, uint64_t *latency_out);

#endif // BRIDGE_H
```

**Step 3: Create `bridge.c`**

```c
// simulator/gosim/csrc/bridge.c
#include "bridge.h"
#include <string.h>

// ── Sim time ─────────────────────────────────────────────────────────
uint64_t g_bridge_sim_time_us = 0;

uint32_t sim_get_time_ms(void) {
    return (uint32_t)(g_bridge_sim_time_us / 1000);
}

// ── Event union accessors ────────────────────────────────────────────

event_type_t bridge_get_event_type(const sim_event_t *evt) {
    return evt->type;
}

uint64_t bridge_get_event_timestamp(const sim_event_t *evt) {
    return evt->timestamp_us;
}

void bridge_get_node_event(const sim_event_t *evt,
    char *node_id_out, uint32_t *addr_out, float *x_out, float *y_out)
{
    strncpy(node_id_out, evt->data.node.node_id, 16);
    *addr_out = evt->data.node.addr;
    *x_out = evt->data.node.x;
    *y_out = evt->data.node.y;
}

void bridge_get_packet_event(const sim_event_t *evt,
    uint32_t *src_out, uint32_t *dest_out, int8_t *rssi_out,
    uint8_t *data_out, uint16_t *len_out)
{
    *src_out = evt->data.packet.src_addr;
    *dest_out = evt->data.packet.dest_addr;
    *rssi_out = evt->data.packet.rssi;
    *len_out = evt->data.packet.len;
    memcpy(data_out, evt->data.packet.data, evt->data.packet.len);
}

void bridge_get_tick_event(const sim_event_t *evt,
    char *node_id_out, uint32_t *tick_seq_out)
{
    strncpy(node_id_out, evt->data.tick.node_id, 16);
    *tick_seq_out = evt->data.tick.tick_seq;
}

void bridge_get_interference_event(const sim_event_t *evt,
    int *zone_idx_out, float *cx_out, float *cy_out, float *radius_out)
{
    *zone_idx_out = evt->data.interference.zone_index;
    *cx_out = evt->data.interference.center_x;
    *cy_out = evt->data.interference.center_y;
    *radius_out = evt->data.interference.radius;
}

// ── Event construction helpers ───────────────────────────────────────

void bridge_make_tick_event(sim_event_t *evt, uint64_t timestamp_us,
    const char *node_id, uint32_t tick_seq)
{
    memset(evt, 0, sizeof(*evt));
    evt->type = EVT_TICK_NODE;
    evt->timestamp_us = timestamp_us;
    strncpy(evt->data.tick.node_id, node_id, 16 - 1);
    evt->data.tick.tick_seq = tick_seq;
}

void bridge_make_node_event(sim_event_t *evt, uint64_t timestamp_us,
    event_type_t type, const char *node_id, uint32_t addr, float x, float y)
{
    memset(evt, 0, sizeof(*evt));
    evt->type = type;
    evt->timestamp_us = timestamp_us;
    strncpy(evt->data.node.node_id, node_id, 16 - 1);
    evt->data.node.addr = addr;
    evt->data.node.x = x;
    evt->data.node.y = y;
}

void bridge_make_generate_msg_event(sim_event_t *evt, uint64_t timestamp_us,
    const char *src_id, uint32_t dest_addr)
{
    memset(evt, 0, sizeof(*evt));
    evt->type = EVT_GENERATE_MESSAGE;
    evt->timestamp_us = timestamp_us;
    strncpy(evt->data.node.node_id, src_id, 16 - 1);
    evt->data.node.addr = dest_addr;
}

void bridge_make_interference_start(sim_event_t *evt, uint64_t timestamp_us,
    float cx, float cy, float radius)
{
    memset(evt, 0, sizeof(*evt));
    evt->type = EVT_INTERFERENCE_START;
    evt->timestamp_us = timestamp_us;
    evt->data.interference.center_x = cx;
    evt->data.interference.center_y = cy;
    evt->data.interference.radius = radius;
}

void bridge_make_interference_end(sim_event_t *evt, uint64_t timestamp_us,
    int zone_index)
{
    memset(evt, 0, sizeof(*evt));
    evt->type = EVT_INTERFERENCE_END;
    evt->timestamp_us = timestamp_us;
    evt->data.interference.zone_index = zone_index;
}

// ── Message tracking ─────────────────────────────────────────────────

void bridge_msg_track_init(msg_tracker_t *track, int count) {
    memset(track, 0, sizeof(msg_tracker_t) * count);
}

void bridge_msg_track_add(msg_tracker_t *track, int count,
    uint32_t packet_id, uint32_t dest_addr, uint64_t sent_us)
{
    for (int i = 0; i < count; i++) {
        if (!track[i].active) {
            track[i].active = 1;
            track[i].packet_id = packet_id;
            track[i].dest_addr = dest_addr;
            track[i].sent_us = sent_us;
            return;
        }
    }
}

int bridge_msg_track_complete(msg_tracker_t *track, int count,
    uint32_t packet_id, uint64_t now_us, uint64_t *latency_out)
{
    for (int i = 0; i < count; i++) {
        if (track[i].active && track[i].packet_id == packet_id) {
            *latency_out = now_us - track[i].sent_us;
            track[i].active = 0;
            return 1;
        }
    }
    return 0;
}

// ── Packet handling ──────────────────────────────────────────────────
// These wrap the complex protocol dispatch logic from main.c so that Go
// can call a single function per event type.

static void _handle_beacon(sim_node_t *rx, const uint8_t *buf, uint16_t len,
    int8_t rssi, uint64_t now_us, uint32_t now_ms,
    node_array_t *nodes, radio_config_t *radio,
    pcg32_state_t *rng, event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly, FILE *out)
{
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, buf, len) != ESP_OK) return;

    neighbor_update(&rx->neighbors, beacon.src_addr, rssi, 0,
                    beacon.pubkey_hash, now_ms);

    route_entry_t *existing = route_lookup(&rx->routes, beacon.src_addr);
    int new_or_broken = (!existing || existing->state == ROUTE_BROKEN);

    uint8_t penalty = compute_link_penalty(rssi, 0);
    uint8_t metric = (penalty >= 255) ? 0 : (uint8_t)(255 - penalty);

    route_install(&rx->routes, beacon.src_addr, beacon.src_addr,
                  1, metric, ROUTE_ACTIVE, now_ms);

    if (new_or_broken) {
        emit_route_added(out, now_us, rx->id,
                         beacon.src_addr, beacon.src_addr, 1);
        int node_idx = (int)(rx - nodes->nodes);
        anomaly_check_route_flap(&anomaly[node_idx].flap,
                                  beacon.src_addr, beacon.src_addr,
                                  now_us, out, rx->id);
    }
    (void)radio; (void)rng; (void)events; (void)metrics;
}

static void _handle_rreq(sim_node_t *rx, const uint8_t *buf, uint16_t len,
    int8_t rssi, uint64_t now_us, uint32_t now_ms,
    node_array_t *nodes, radio_config_t *radio,
    pcg32_state_t *rng, event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly, FILE *out)
{
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

        int node_idx = (int)(rx - nodes->nodes);
        anomaly_check_rreq_retx(&anomaly[node_idx].rreq_retx,
                                 rreq.header.dest_addr, now_us, out, rx->id);

        sim_radio_broadcast(rx, &pkt, nodes, radio, rng, events, metrics, now_us);
    }
}

static void _handle_rrep(sim_node_t *rx, const uint8_t *buf, uint16_t len,
    uint32_t pkt_src_addr, uint64_t now_us, uint32_t now_ms,
    node_array_t *nodes, radio_config_t *radio,
    pcg32_state_t *rng, event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly, FILE *out)
{
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, buf, len) != ESP_OK) return;

    route_install(&rx->routes, rrep.src_addr, pkt_src_addr,
                  rrep.hop_count, rrep.route_metric, ROUTE_ACTIVE, now_ms);
    emit_route_added(out, now_us, rx->id,
                     rrep.src_addr, pkt_src_addr, rrep.hop_count);

    int node_idx = (int)(rx - nodes->nodes);
    anomaly_check_route_flap(&anomaly[node_idx].flap,
                              rrep.src_addr, pkt_src_addr,
                              now_us, out, rx->id);

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
    uint64_t now_us, uint32_t now_ms, FILE *out)
{
    (void)now_ms; (void)out;
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, buf, len) != ESP_OK) return;
    rerr_handle(&rx->routes, &rerr);
    emit_link_broken(out, now_us, rx->id, rerr.broken_next_hop);
}

static void _handle_data(sim_node_t *rx, const uint8_t *buf, uint16_t len,
    uint64_t now_us, uint32_t now_ms,
    node_array_t *nodes, radio_config_t *radio,
    pcg32_state_t *rng, event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    msg_tracker_t *msg_track, int msg_track_count,
    FILE *out)
{
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) return;

    int node_idx = (int)(rx - nodes->nodes);

    anomaly_record_rx(&anomaly[node_idx].blackhole, now_us);
    anomaly_check_loop(&anomaly[node_idx].loop, hdr.packet_id, now_us, out, rx->id);

    if (hdr.dest_addr == rx->addr) {
        // Delivery
        uint64_t latency = 0;
        if (bridge_msg_track_complete(msg_track, msg_track_count,
                                       hdr.packet_id, now_us, &latency)) {
            metrics_record_packet_delivered(metrics, latency);
        }
        fprintf(out,
            "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"packet_id\":\"0x%08X\"}\n",
            (unsigned long long)now_us, rx->id, hdr.packet_id);
        fflush(out);
        anomaly_record_fwd(&anomaly[node_idx].blackhole, now_us);
        return;
    }

    // Forward
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
        emit_packet_dropped(out, now_us, rx->id, "no_route");
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

void bridge_handle_receive_packet(
    sim_event_t *evt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    FILE *out)
{
    sim_node_t *rx = node_array_find_by_addr(nodes, evt->data.packet.dest_addr);
    if (!rx || !rx->active) return;

    uint32_t now_ms = (uint32_t)(evt->timestamp_us / 1000);
    const uint8_t *buf = evt->data.packet.data;
    uint16_t len = evt->data.packet.len;
    int8_t rssi = evt->data.packet.rssi;

    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) return;

    emit_packet_received_typed(out, evt->timestamp_us, rx->id,
                               evt->data.packet.src_addr, rssi, len, hdr.type);
    rx->packets_received++;

    switch (hdr.type) {
        case PKT_TYPE_BEACON:
            _handle_beacon(rx, buf, len, rssi, evt->timestamp_us, now_ms,
                          nodes, radio, rng, events, metrics, anomaly, out);
            break;
        case PKT_TYPE_RREQ:
            _handle_rreq(rx, buf, len, rssi, evt->timestamp_us, now_ms,
                        nodes, radio, rng, events, metrics, anomaly, out);
            break;
        case PKT_TYPE_RREP:
            _handle_rrep(rx, buf, len, evt->data.packet.src_addr,
                        evt->timestamp_us, now_ms,
                        nodes, radio, rng, events, metrics, anomaly, out);
            break;
        case PKT_TYPE_RERR:
            _handle_rerr(rx, buf, len, evt->timestamp_us, now_ms, out);
            break;
        case PKT_TYPE_DATA:
            _handle_data(rx, buf, len, evt->timestamp_us, now_ms,
                        nodes, radio, rng, events, metrics, anomaly,
                        NULL, 0, out);  // msg_track passed separately
            break;
        default:
            break;
    }
}

void bridge_handle_generate_message(
    sim_event_t *evt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    node_anomaly_tracker_t *anomaly,
    FILE *out)
{
    sim_node_t *src = node_array_find_by_id(nodes, evt->data.node.node_id);
    if (!src || !src->active) return;

    uint32_t dest_addr = evt->data.node.addr;
    uint32_t now_ms = (uint32_t)(evt->timestamp_us / 1000);

    route_entry_t *route = route_lookup(&src->routes, dest_addr);

    if (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING) {
        pending_discovery_t *pd = discovery_lookup(&src->pending_discoveries, dest_addr);
        if (pd && (now_ms - pd->timestamp > 5000)) {
            discovery_remove(&src->pending_discoveries, dest_addr);
            pd = NULL;
        }
        int should_send_rreq = 0;
        if (!pd) {
            uint32_t query_id = pcg32_random(rng);
            discovery_start(&src->pending_discoveries, dest_addr, query_id, now_ms);
            should_send_rreq = 1;
        } else if (pd->attempts < MAX_RREQ_ATTEMPTS && (now_ms - pd->timestamp) > 1000) {
            pd->attempts++;
            pd->query_id = pcg32_random(rng);
            pd->timestamp = now_ms;
            should_send_rreq = 1;
        }

        if (should_send_rreq) {
            pd = discovery_lookup(&src->pending_discoveries, dest_addr);
            bramble_rreq_t rreq = rreq_build_originator(src->addr, dest_addr,
                                                          pd->query_id, src->addr);
            rreq.header.hop_limit = 32;

            outbound_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            bramble_rreq_serialize(&rreq, pkt.data, RREQ_SIZE);
            pkt.len = RREQ_SIZE;
            pkt.is_broadcast = true;
            pkt.dest_addr = 0xFFFFFFFF;
            pkt.pkt_type = PKT_TYPE_RREQ;

            sim_radio_broadcast(src, &pkt, nodes, radio, rng, events, metrics, evt->timestamp_us);

            int src_idx = (int)(src - nodes->nodes);
            anomaly_check_rreq_retx(&anomaly[src_idx].rreq_retx,
                                     dest_addr, evt->timestamp_us, out, src->id);
        }

        // Reschedule
        sim_event_t retry = *evt;
        retry.timestamp_us += 1500000ULL;
        event_queue_push(events, &retry);
        return;
    }

    // Route exists — send DATA
    uint8_t hop_limit = 32;
    forward_result_t fwd_res = forward_data(&src->routes, dest_addr, &hop_limit, now_ms);
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

    outbound_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.data, data_buf, HEADER_SIZE);
    pkt.len = HEADER_SIZE;
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;
    src->packets_originated++;

    sim_radio_broadcast(src, &pkt, nodes, radio, rng, events, metrics, evt->timestamp_us);

    metrics_record_message_sent(metrics);
    fprintf(out,
        "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"packet_id\":\"0x%08X\"}\n",
        (unsigned long long)evt->timestamp_us,
        src->id, dest_addr, hdr.packet_id);
    fflush(out);
}
```

**Step 4: Verify the C code compiles (standalone test)**

Run: `cd simulator/gosim/csrc && gcc -c -DBRAMBLE_SIM -I../../../test/stubs -I../../../components/routing/include -I../../../components/packet/include -I../../../components/dedup/include -I../../../components/crypto/include -I../../../components/reliability/include -I../../../components/fragment/include -I../../../components/channel/include -I../../../components/security/include -I../../../components/airtime/include -I../../../components/timesync/include all.c -o /dev/null 2>&1`
Expected: Compiles with no errors (warnings OK).

**Step 5: Commit**

```bash
git add simulator/gosim/csrc/
git commit -m "feat(sim): add C bridge layer for cgo integration"
```

---

### Task 3: Create Go cgo bridge bindings

**Files:**
- Create: `simulator/gosim/bridge.go`

This file contains the cgo directives and Go wrappers around the C bridge functions.

**Step 1: Create `bridge.go`**

```go
// simulator/gosim/bridge.go
package main

/*
#cgo CFLAGS: -DBRAMBLE_SIM -std=c11 -O2
#cgo CFLAGS: -I${SRCDIR}/csrc
#cgo CFLAGS: -I${SRCDIR}/../engine
#cgo CFLAGS: -I${SRCDIR}/../../test/stubs
#cgo CFLAGS: -I${SRCDIR}/../../components/routing/include
#cgo CFLAGS: -I${SRCDIR}/../../components/packet/include
#cgo CFLAGS: -I${SRCDIR}/../../components/dedup/include
#cgo CFLAGS: -I${SRCDIR}/../../components/crypto/include
#cgo CFLAGS: -I${SRCDIR}/../../components/reliability/include
#cgo CFLAGS: -I${SRCDIR}/../../components/fragment/include
#cgo CFLAGS: -I${SRCDIR}/../../components/channel/include
#cgo CFLAGS: -I${SRCDIR}/../../components/security/include
#cgo CFLAGS: -I${SRCDIR}/../../components/airtime/include
#cgo CFLAGS: -I${SRCDIR}/../../components/timesync/include
#cgo LDFLAGS: -lm

#include "csrc/bridge.h"
#include "csrc/all.c"
*/
import "C"
import (
	"unsafe"
)

// setSimTime updates the global sim time visible to C code.
func setSimTime(us uint64) {
	C.g_bridge_sim_time_us = C.uint64_t(us)
}

// --- Scenario loading ---

type Scenario struct {
	Name        string
	Seed        uint64
	DurationUS  uint64
	Deterministic bool
}

func loadScenario(path string, nodes *C.node_array_t, radio *C.radio_config_t,
	events *C.event_queue_t, rng *C.pcg32_state_t) (*Scenario, bool) {

	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	var scenario C.scenario_t
	scenario.nodes = nodes
	scenario.radio = radio
	scenario.events = events
	scenario.rng = rng

	if C.scenario_load_file(cPath, &scenario) != true {
		return nil, false
	}

	return &Scenario{
		Name:          C.GoString(&scenario.metadata.name[0]),
		Seed:          uint64(scenario.metadata.seed),
		DurationUS:    uint64(scenario.metadata.duration_us),
		Deterministic: bool(scenario.metadata.deterministic),
	}, true
}

// --- Node operations ---

func nodeArrayInit(nodes *C.node_array_t) {
	C.node_array_init(nodes)
}

func nodeArrayAdd(nodes *C.node_array_t, id string, addr uint32, x, y float32) int {
	cID := C.CString(id)
	defer C.free(unsafe.Pointer(cID))
	return int(C.node_array_add(nodes, cID, C.uint32_t(addr), C.float(x), C.float(y)))
}

func nodeArrayFindByID(nodes *C.node_array_t, id string) *C.sim_node_t {
	cID := C.CString(id)
	defer C.free(unsafe.Pointer(cID))
	return C.node_array_find_by_id(nodes, cID)
}

func nodeCount(nodes *C.node_array_t) int {
	return int(nodes.count)
}

func nodeActivate(node *C.sim_node_t) {
	C.node_activate(node)
}

func nodeDeactivate(node *C.sim_node_t) {
	C.node_deactivate(node)
}

func nodeMove(node *C.sim_node_t, x, y float32) {
	C.node_move(node, C.float(x), C.float(y))
}

// --- Event queue ---

func eventQueueInit(q *C.event_queue_t) {
	C.event_queue_init(q)
}

func eventQueuePush(q *C.event_queue_t, evt *C.sim_event_t) bool {
	return bool(C.event_queue_push(q, evt))
}

func eventQueuePop(q *C.event_queue_t, evt *C.sim_event_t) bool {
	return bool(C.event_queue_pop(q, evt))
}

func eventQueuePeek(q *C.event_queue_t) *C.sim_event_t {
	return C.event_queue_peek(q)
}

func eventQueueCount(q *C.event_queue_t) int {
	return int(C.event_queue_count(q))
}

// --- Event accessors ---

func getEventType(evt *C.sim_event_t) C.event_type_t {
	return C.bridge_get_event_type(evt)
}

func getEventTimestamp(evt *C.sim_event_t) uint64 {
	return uint64(C.bridge_get_event_timestamp(evt))
}

// --- Radio ---

func radioConfigInit(r *C.radio_config_t) {
	C.radio_config_init(r)
}

func radioAddInterference(r *C.radio_config_t, cx, cy, radius float32) int {
	return int(C.radio_add_interference_zone(r, C.float(cx), C.float(cy), C.float(radius)))
}

func radioClearInterference(r *C.radio_config_t, idx int) {
	C.radio_clear_interference_zone(r, C.int(idx))
}

// --- Metrics ---

func metricsInit(m *C.metrics_state_t) {
	C.metrics_init(m)
}

func metricsUpdateActiveNodes(m *C.metrics_state_t, count int) {
	C.metrics_update_active_nodes(m, C.int(count))
}

func metricsDeliveryRate(m *C.metrics_state_t) float64 {
	return float64(C.metrics_delivery_rate(m))
}

func metricsAvgLatencyMs(m *C.metrics_state_t) float64 {
	return float64(C.metrics_avg_latency_ms(m))
}

// --- Anomaly ---

func anomalyInit(a *C.node_anomaly_tracker_t) {
	C.anomaly_init(a)
}

func anomalyCheckPartition(nodes *C.node_array_t, radioRange float32,
	nowUS uint64, out *C.FILE) {
	C.anomaly_check_partition(nodes, C.float(radioRange), C.uint64_t(nowUS), out)
}

// --- RNG ---

func pcg32Seed(rng *C.pcg32_state_t, seed uint64) {
	C.pcg32_seed(rng, C.uint64_t(seed))
}
```

**Step 2: Test that cgo compiles**

Run: `cd simulator/gosim && go build -o bramble-gosim . 2>&1`
Expected: Compiles successfully (this validates the entire C + Go integration).

**Step 3: Commit**

```bash
git add simulator/gosim/bridge.go
git commit -m "feat(sim): add Go cgo bridge bindings"
```

---

## Phase 2: Simulation Engine in Go

### Task 4: Create the simulation state machine

**Files:**
- Create: `simulator/gosim/sim.go`

This is the core simulation engine. It manages state (IDLE/LOADED/RUNNING/PAUSED/COMPLETED), the event loop, and command processing.

**Step 1: Create `sim.go`**

```go
// simulator/gosim/sim.go
package main

/*
#include "csrc/bridge.h"
#include <stdio.h>
#include <stdlib.h>
*/
import "C"
import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"os"
	"sync"
	"time"
	"unsafe"
)

type SimState int

const (
	StateIdle SimState = iota
	StateLoaded
	StateRunning
	StatePaused
	StateCompleted
)

func (s SimState) String() string {
	switch s {
	case StateIdle:
		return "idle"
	case StateLoaded:
		return "loaded"
	case StateRunning:
		return "running"
	case StatePaused:
		return "paused"
	case StateCompleted:
		return "completed"
	default:
		return "unknown"
	}
}

type Command struct {
	Type    string          `json:"type"`
	Payload json.RawMessage `json:"payload,omitempty"`

	// Parsed fields (populated by command handlers)
	Scenario string  `json:"scenario,omitempty"`
	Value    float64 `json:"value,omitempty"`
	NodeID   string  `json:"node_id,omitempty"`
	X        float32 `json:"x,omitempty"`
	Y        float32 `json:"y,omitempty"`
	Src      string  `json:"src,omitempty"`
	Dest     string  `json:"dest,omitempty"`
	Radius   float32 `json:"radius,omitempty"`
}

type Sim struct {
	mu sync.RWMutex

	state    SimState
	scenario *Scenario

	// C state (owned by sim, never accessed outside sim goroutine)
	nodes   C.node_array_t
	radio   C.radio_config_t
	events  C.event_queue_t
	rng     C.pcg32_state_t
	metrics C.metrics_state_t
	anomaly [C.MAX_NODES]C.node_anomaly_tracker_t
	msgTrack [C.MAX_MSG_TRACK]C.msg_tracker_t

	// Simulation clock
	simTime      uint64 // current sim time in microseconds
	duration     uint64 // scenario duration in microseconds
	speed        float64
	wallStart    time.Time
	simAtStart   uint64 // sim time when play started/resumed

	// Pipe for capturing C emitter output
	pipeR    *os.File
	pipeW    *os.File
	emitFile *C.FILE

	// Address counter for dynamically added nodes
	nextAddr uint32

	// Channels
	cmdCh  chan Command
	stopCh chan struct{}

	// Broadcaster (set externally)
	broadcast func([]byte)

	// Scenario directory
	scenarioDir string

	// Headless mode
	headless bool
}

func NewSim(scenarioDir string, broadcast func([]byte), headless bool) *Sim {
	s := &Sim{
		state:       StateIdle,
		speed:       1.0,
		cmdCh:       make(chan Command, 64),
		stopCh:      make(chan struct{}),
		broadcast:   broadcast,
		scenarioDir: scenarioDir,
		headless:    headless,
		nextAddr:    0x03000000, // dynamic nodes get a distinct address range
	}

	// Create pipe for C event output
	r, w, err := os.Pipe()
	if err != nil {
		log.Fatalf("Failed to create pipe: %v", err)
	}
	s.pipeR = r
	s.pipeW = w

	// Open the write end as a C FILE*
	cMode := C.CString("w")
	defer C.free(unsafe.Pointer(cMode))
	s.emitFile = C.fdopen(C.int(w.Fd()), cMode)
	if s.emitFile == nil {
		log.Fatal("Failed to fdopen pipe")
	}

	return s
}

func (s *Sim) Start() {
	// Start pipe reader goroutine
	go s.readPipe()
	// Start simulation goroutine
	go s.run()
}

func (s *Sim) Stop() {
	close(s.stopCh)
	C.fclose(s.emitFile)
	s.pipeW.Close()
	s.pipeR.Close()
}

func (s *Sim) SendCommand(cmd Command) {
	select {
	case s.cmdCh <- cmd:
	default:
		log.Println("[sim] Command channel full, dropping:", cmd.Type)
	}
}

func (s *Sim) GetState() SimState {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.state
}

func (s *Sim) setState(st SimState) {
	s.mu.Lock()
	s.state = st
	s.mu.Unlock()
}

// readPipe reads JSON lines from the C emitter pipe and broadcasts them.
func (s *Sim) readPipe() {
	scanner := bufio.NewScanner(s.pipeR)
	// Increase buffer for large JSON lines
	buf := make([]byte, 0, 64*1024)
	scanner.Buffer(buf, 1024*1024)

	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}

		// Apply event filter (skip BEACON/RREQ packet events)
		if shouldFilter(line) {
			continue
		}

		// Make a copy since scanner reuses the buffer
		msg := make([]byte, len(line))
		copy(msg, line)

		if s.headless {
			os.Stdout.Write(msg)
			os.Stdout.Write([]byte("\n"))
		} else {
			s.broadcast(msg)
		}
	}
}

// shouldFilter returns true if this event should be filtered out.
func shouldFilter(line []byte) bool {
	// Quick check: only filter packet_sent and packet_received
	// Look for "pkt_type":"BEACON" or "pkt_type":"RREQ"
	if len(line) < 20 {
		return false
	}

	// Fast path: check if it's a packet event
	hasPacketSent := false
	hasPacketRecv := false
	for i := 0; i < len(line)-12; i++ {
		if line[i] == 'p' && line[i+1] == 'a' && line[i+2] == 'c' && line[i+3] == 'k' {
			if i+11 < len(line) && line[i+7] == 's' {
				hasPacketSent = true
			}
			if i+15 < len(line) && line[i+7] == 'r' {
				hasPacketRecv = true
			}
		}
	}

	if !hasPacketSent && !hasPacketRecv {
		return false
	}

	// Check for BEACON or RREQ
	for i := 0; i < len(line)-8; i++ {
		if line[i] == 'B' && line[i+1] == 'E' && line[i+2] == 'A' && line[i+3] == 'C' {
			return true
		}
		if line[i] == 'R' && line[i+1] == 'R' && line[i+2] == 'E' && line[i+3] == 'Q' {
			return true
		}
	}

	return false
}

// run is the main simulation goroutine.
func (s *Sim) run() {
	ticker := time.NewTicker(time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-s.stopCh:
			return

		case cmd := <-s.cmdCh:
			s.handleCommand(cmd)

		case <-ticker.C:
			if s.state != StateRunning {
				continue
			}
			s.advanceSim()
		}
	}
}

// advanceSim processes events up to the current effective sim time.
func (s *Sim) advanceSim() {
	wallElapsed := time.Since(s.wallStart)
	simElapsedUS := uint64(float64(wallElapsed.Microseconds()) * s.speed)
	simNow := s.simAtStart + simElapsedUS

	// Cap at duration
	if simNow > s.duration {
		simNow = s.duration
	}

	// Process events up to simNow
	var evt C.sim_event_t
	for {
		peek := eventQueuePeek(&s.events)
		if peek == nil {
			break
		}
		peekTS := uint64(peek.timestamp_us)
		if peekTS > simNow {
			break
		}
		if !eventQueuePop(&s.events, &evt) {
			break
		}
		s.simTime = uint64(evt.timestamp_us)
		setSimTime(s.simTime)
		s.dispatchEvent(&evt)
	}

	// Check completion
	if s.simTime >= s.duration {
		s.complete()
	}
}

// handleCommand processes a command from a WebSocket client.
func (s *Sim) handleCommand(cmd Command) {
	switch cmd.Type {
	case "start", "load":
		s.cmdLoad(cmd)
	case "play":
		s.cmdPlay()
	case "pause":
		s.cmdPause()
	case "restart":
		s.cmdRestart()
	case "speed":
		s.cmdSpeed(cmd)
	case "instant":
		s.cmdInstant()
	case "add_node":
		s.cmdAddNode(cmd)
	case "remove_node":
		s.cmdRemoveNode(cmd)
	case "move_node":
		s.cmdMoveNode(cmd)
	case "send_message":
		s.cmdSendMessage(cmd)
	case "interference":
		s.cmdInterference(cmd)
	default:
		log.Printf("[sim] Unknown command: %s", cmd.Type)
	}
}

func (s *Sim) cmdLoad(cmd Command) {
	scenarioName := cmd.Scenario
	if scenarioName == "" {
		scenarioName = "10-node-grid"
	}
	path := fmt.Sprintf("%s/%s.json", s.scenarioDir, scenarioName)

	// Reset C state
	nodeArrayInit(&s.nodes)
	eventQueueInit(&s.events)
	metricsInit(&s.metrics)
	C.bridge_msg_track_init(&s.msgTrack[0], C.MAX_MSG_TRACK)
	for i := 0; i < int(C.MAX_NODES); i++ {
		anomalyInit(&s.anomaly[i])
	}

	meta, ok := loadScenario(path, &s.nodes, &s.radio, &s.events, &s.rng)
	if !ok {
		errMsg := fmt.Sprintf(`{"type":"error","message":"Failed to load scenario: %s"}`, path)
		s.broadcast([]byte(errMsg))
		return
	}

	s.scenario = meta
	s.duration = meta.DurationUS
	s.simTime = 0
	s.nextAddr = 0x03000000

	pcg32Seed(&s.rng, meta.Seed)

	// Schedule initial node ticks
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node == nil || !node.active {
			continue
		}
		var tickEvt C.sim_event_t
		cID := C.GoString(&node.id[0])
		cIDc := C.CString(cID)
		C.bridge_make_tick_event(&tickEvt, C.uint64_t(uint64(i)*100000), cIDc, 0)
		C.free(unsafe.Pointer(cIDc))
		eventQueuePush(&s.events, &tickEvt)
	}

	// Send sim_reset to clients
	s.broadcast([]byte(`{"type":"sim_reset","timestamp_us":0}`))

	// Send initial node positions
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node == nil || !node.active {
			continue
		}
		msg := fmt.Sprintf(
			`{"type":"node_joined","timestamp_us":0,"node":"%s","addr":"0x%08X","x":%.2f,"y":%.2f}`,
			C.GoString(&node.id[0]), uint32(node.addr), float32(node.x), float32(node.y))
		s.broadcast([]byte(msg))
	}

	// Send radio config
	s.broadcast([]byte(fmt.Sprintf(
		`{"type":"config","timestamp_us":0,"radio_range":%.2f}`,
		float32(s.radio._range))))

	// Send sim_ready
	s.broadcast([]byte(fmt.Sprintf(
		`{"type":"sim_ready","total_events":%d,"timestamp_us":0}`,
		eventQueueCount(&s.events))))

	s.setState(StateLoaded)
	log.Printf("[sim] Loaded scenario '%s' (%d nodes, %d events, %dms duration)",
		meta.Name, nodeCount(&s.nodes), eventQueueCount(&s.events), meta.DurationUS/1000)
}

func (s *Sim) cmdPlay() {
	if s.state != StateLoaded && s.state != StatePaused {
		return
	}
	s.wallStart = time.Now()
	s.simAtStart = s.simTime
	s.setState(StateRunning)
	log.Println("[sim] Playing")
}

func (s *Sim) cmdPause() {
	if s.state != StateRunning {
		return
	}
	s.setState(StatePaused)
	log.Println("[sim] Paused at", s.simTime/1000, "ms")
}

func (s *Sim) cmdRestart() {
	if s.scenario == nil {
		return
	}
	// Re-load current scenario
	s.cmdLoad(Command{Type: "load", Scenario: s.scenario.Name})
}

func (s *Sim) cmdSpeed(cmd Command) {
	newSpeed := cmd.Value
	if newSpeed < 0.1 {
		newSpeed = 0.1
	}
	if newSpeed > 1000 {
		newSpeed = 1000
	}

	if s.state == StateRunning {
		// Re-anchor
		wallElapsed := time.Since(s.wallStart)
		simElapsedUS := uint64(float64(wallElapsed.Microseconds()) * s.speed)
		s.simAtStart += simElapsedUS
		s.wallStart = time.Now()
	}
	s.speed = newSpeed
	log.Printf("[sim] Speed set to %.1fx", newSpeed)
}

func (s *Sim) cmdInstant() {
	if s.state != StateLoaded && s.state != StatePaused && s.state != StateRunning {
		return
	}

	log.Println("[sim] Running to completion (instant mode)")

	// Process all remaining events
	var evt C.sim_event_t
	for eventQueuePop(&s.events, &evt) {
		ts := uint64(evt.timestamp_us)
		if ts > s.duration {
			break
		}
		s.simTime = ts
		setSimTime(s.simTime)
		s.dispatchEvent(&evt)
	}
	s.complete()
}

func (s *Sim) cmdAddNode(cmd Command) {
	if s.state != StateRunning && s.state != StatePaused && s.state != StateLoaded {
		return
	}

	id := cmd.NodeID
	x := cmd.X
	y := cmd.Y

	if id == "" {
		// Auto-generate ID
		id = fmt.Sprintf("X%d", nodeCount(&s.nodes))
	}

	addr := s.nextAddr
	s.nextAddr++

	idx := nodeArrayAdd(&s.nodes, id, addr, x, y)
	if idx < 0 {
		log.Println("[sim] Failed to add node (array full)")
		return
	}

	// Init anomaly tracker
	anomalyInit(&s.anomaly[idx])

	// Schedule first tick
	var tickEvt C.sim_event_t
	cID := C.CString(id)
	C.bridge_make_tick_event(&tickEvt, C.uint64_t(s.simTime+100000), cID, 0)
	C.free(unsafe.Pointer(cID))
	eventQueuePush(&s.events, &tickEvt)

	// Emit node_joined
	msg := fmt.Sprintf(
		`{"type":"node_joined","timestamp_us":%d,"node":"%s","addr":"0x%08X","x":%.2f,"y":%.2f}`,
		s.simTime, id, addr, x, y)
	s.broadcast([]byte(msg))

	log.Printf("[sim] Added node %s at (%.1f, %.1f)", id, x, y)
}

func (s *Sim) cmdRemoveNode(cmd Command) {
	if s.state != StateRunning && s.state != StatePaused {
		return
	}
	node := nodeArrayFindByID(&s.nodes, cmd.NodeID)
	if node == nil {
		return
	}
	nodeDeactivate(node)

	msg := fmt.Sprintf(`{"type":"node_left","timestamp_us":%d,"node":"%s"}`,
		s.simTime, cmd.NodeID)
	s.broadcast([]byte(msg))

	anomalyCheckPartition(&s.nodes, float32(s.radio._range), s.simTime, s.emitFile)
	log.Printf("[sim] Removed node %s", cmd.NodeID)
}

func (s *Sim) cmdMoveNode(cmd Command) {
	node := nodeArrayFindByID(&s.nodes, cmd.NodeID)
	if node == nil {
		return
	}
	nodeMove(node, cmd.X, cmd.Y)

	msg := fmt.Sprintf(
		`{"type":"node_moved","timestamp_us":%d,"node":"%s","x":%.2f,"y":%.2f}`,
		s.simTime, cmd.NodeID, cmd.X, cmd.Y)
	s.broadcast([]byte(msg))
}

func (s *Sim) cmdSendMessage(cmd Command) {
	if s.state != StateRunning && s.state != StatePaused {
		return
	}

	srcNode := nodeArrayFindByID(&s.nodes, cmd.Src)
	destNode := nodeArrayFindByID(&s.nodes, cmd.Dest)
	if srcNode == nil || destNode == nil {
		return
	}

	var evt C.sim_event_t
	cSrc := C.CString(cmd.Src)
	C.bridge_make_generate_msg_event(&evt, C.uint64_t(s.simTime), cSrc, C.uint32_t(destNode.addr))
	C.free(unsafe.Pointer(cSrc))
	eventQueuePush(&s.events, &evt)

	log.Printf("[sim] Injected message %s → %s", cmd.Src, cmd.Dest)
}

func (s *Sim) cmdInterference(cmd Command) {
	// Add interference zone
	radioAddInterference(&s.radio, cmd.X, cmd.Y, cmd.Radius)
	log.Printf("[sim] Added interference at (%.1f, %.1f) radius %.1f", cmd.X, cmd.Y, cmd.Radius)
}

// complete emits final metrics and transitions to completed state.
func (s *Sim) complete() {
	// Emit final metrics
	active := 0
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && node.active {
			active++
		}
	}
	metricsUpdateActiveNodes(&s.metrics, active)

	msg := fmt.Sprintf(
		`{"type":"metrics","timestamp_us":%d,"active_nodes":%d,"total_packets":%d,"messages_sent":%d,"delivered":%d,"dropped":%d,"avg_latency_ms":%.3f}`,
		s.simTime, active,
		uint64(s.metrics.total_packets),
		uint64(s.metrics.messages_sent),
		uint64(s.metrics.delivered_packets),
		uint64(s.metrics.dropped_packets),
		metricsAvgLatencyMs(&s.metrics))
	s.broadcast([]byte(msg))

	// Send sim_ended
	s.broadcast([]byte(fmt.Sprintf(
		`{"type":"sim_ended","timestamp_us":%d,"code":0}`, s.simTime)))

	s.setState(StateCompleted)

	log.Printf("[sim] Complete. Packets: %d, Delivered: %d/%d (%.1f%%), Latency: %.1fms",
		uint64(s.metrics.total_packets),
		uint64(s.metrics.delivered_packets),
		uint64(s.metrics.messages_sent),
		metricsDeliveryRate(&s.metrics)*100,
		metricsAvgLatencyMs(&s.metrics))
}

// dispatchEvent routes a C event to the appropriate handler.
func (s *Sim) dispatchEvent(evt *C.sim_event_t) {
	evtType := getEventType(evt)

	switch evtType {
	case C.EVT_TICK_NODE:
		s.handleTickNode(evt)
	case C.EVT_RECEIVE_PACKET:
		s.handleReceivePacket(evt)
	case C.EVT_GENERATE_MESSAGE:
		s.handleGenerateMessage(evt)
	case C.EVT_NODE_JOIN:
		s.handleNodeJoin(evt)
	case C.EVT_NODE_LEAVE:
		s.handleNodeLeave(evt)
	case C.EVT_NODE_MOVE:
		s.handleNodeMove(evt)
	case C.EVT_INTERFERENCE_START:
		s.handleInterferenceStart(evt)
	case C.EVT_INTERFERENCE_END:
		s.handleInterferenceEnd(evt)
	case C.EVT_METRICS_TICK:
		s.handleMetricsTick(evt)
	}
}

func (s *Sim) handleTickNode(evt *C.sim_event_t) {
	var nodeID [16]C.char
	var tickSeq C.uint32_t
	C.bridge_get_tick_event(evt, &nodeID[0], &tickSeq)

	goID := C.GoString(&nodeID[0])
	node := nodeArrayFindByID(&s.nodes, goID)
	if node == nil || !node.active {
		return
	}

	// Call node_tick
	var result C.node_tick_result_t
	C.node_tick(node, C.uint64_t(s.simTime), &result)

	// Broadcast outbound packets
	for i := 0; i < int(result.count); i++ {
		pkt := &result.pkts[i]
		C.sim_radio_broadcast(node, pkt, &s.nodes, &s.radio, &s.rng,
			&s.events, &s.metrics, C.uint64_t(s.simTime))
	}

	// Reschedule next tick
	var nextTick C.sim_event_t
	cID := C.CString(goID)
	C.bridge_make_tick_event(&nextTick,
		C.uint64_t(s.simTime+uint64(C.NODE_TICK_INTERVAL_US)),
		cID, tickSeq+1)
	C.free(unsafe.Pointer(cID))
	eventQueuePush(&s.events, &nextTick)
}

func (s *Sim) handleReceivePacket(evt *C.sim_event_t) {
	C.bridge_handle_receive_packet(evt, &s.nodes, &s.radio, &s.rng,
		&s.events, &s.metrics, &s.anomaly[0], s.emitFile)
}

func (s *Sim) handleGenerateMessage(evt *C.sim_event_t) {
	C.bridge_handle_generate_message(evt, &s.nodes, &s.radio, &s.rng,
		&s.events, &s.metrics, &s.anomaly[0], s.emitFile)
}

func (s *Sim) handleNodeJoin(evt *C.sim_event_t) {
	var nodeID [16]C.char
	var addr C.uint32_t
	var x, y C.float
	C.bridge_get_node_event(evt, &nodeID[0], &addr, &x, &y)

	goID := C.GoString(&nodeID[0])
	node := nodeArrayFindByID(&s.nodes, goID)
	if node != nil {
		nodeActivate(node)
		msg := fmt.Sprintf(
			`{"type":"node_joined","timestamp_us":%d,"node":"%s","addr":"0x%08X","x":%.2f,"y":%.2f}`,
			s.simTime, goID, uint32(node.addr), float32(node.x), float32(node.y))
		s.broadcast([]byte(msg))
	}
}

func (s *Sim) handleNodeLeave(evt *C.sim_event_t) {
	var nodeID [16]C.char
	var addr C.uint32_t
	var x, y C.float
	C.bridge_get_node_event(evt, &nodeID[0], &addr, &x, &y)

	goID := C.GoString(&nodeID[0])
	node := nodeArrayFindByID(&s.nodes, goID)
	if node != nil {
		nodeDeactivate(node)
		msg := fmt.Sprintf(`{"type":"node_left","timestamp_us":%d,"node":"%s"}`,
			s.simTime, goID)
		s.broadcast([]byte(msg))
		anomalyCheckPartition(&s.nodes, float32(s.radio._range), s.simTime, s.emitFile)
	}
}

func (s *Sim) handleNodeMove(evt *C.sim_event_t) {
	var nodeID [16]C.char
	var addr C.uint32_t
	var x, y C.float
	C.bridge_get_node_event(evt, &nodeID[0], &addr, &x, &y)

	goID := C.GoString(&nodeID[0])
	node := nodeArrayFindByID(&s.nodes, goID)
	if node != nil {
		nodeMove(node, float32(x), float32(y))
		msg := fmt.Sprintf(
			`{"type":"node_moved","timestamp_us":%d,"node":"%s","x":%.2f,"y":%.2f}`,
			s.simTime, goID, float32(x), float32(y))
		s.broadcast([]byte(msg))
	}
}

func (s *Sim) handleInterferenceStart(evt *C.sim_event_t) {
	var zoneIdx C.int
	var cx, cy, radius C.float
	C.bridge_get_interference_event(evt, &zoneIdx, &cx, &cy, &radius)
	radioAddInterference(&s.radio, float32(cx), float32(cy), float32(radius))
}

func (s *Sim) handleInterferenceEnd(evt *C.sim_event_t) {
	var zoneIdx C.int
	var cx, cy, radius C.float
	C.bridge_get_interference_event(evt, &zoneIdx, &cx, &cy, &radius)
	if int(zoneIdx) >= 0 {
		radioClearInterference(&s.radio, int(zoneIdx))
	}
}

func (s *Sim) handleMetricsTick(evt *C.sim_event_t) {
	active := 0
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && node.active {
			active++
		}
	}
	metricsUpdateActiveNodes(&s.metrics, active)

	msg := fmt.Sprintf(
		`{"type":"metrics","timestamp_us":%d,"active_nodes":%d,"total_packets":%d,"messages_sent":%d,"delivered":%d,"dropped":%d,"avg_latency_ms":%.3f}`,
		s.simTime, active,
		uint64(s.metrics.total_packets),
		uint64(s.metrics.messages_sent),
		uint64(s.metrics.delivered_packets),
		uint64(s.metrics.dropped_packets),
		metricsAvgLatencyMs(&s.metrics))
	s.broadcast([]byte(msg))

	// Check black holes
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && node.active {
			C.anomaly_check_blackhole(&s.anomaly[i].blackhole,
				C.uint64_t(s.simTime), s.emitFile,
				&node.id[0])
		}
	}
}

// RunHeadless loads a scenario and runs it to completion immediately.
// Output goes to stdout. Returns exit code.
func (s *Sim) RunHeadless(scenarioPath string) int {
	// For headless, broadcast = stdout (already handled in readPipe)

	// Load
	nodeArrayInit(&s.nodes)
	eventQueueInit(&s.events)
	metricsInit(&s.metrics)
	C.bridge_msg_track_init(&s.msgTrack[0], C.MAX_MSG_TRACK)
	for i := 0; i < int(C.MAX_NODES); i++ {
		anomalyInit(&s.anomaly[i])
	}

	meta, ok := loadScenario(scenarioPath, &s.nodes, &s.radio, &s.events, &s.rng)
	if !ok {
		fmt.Fprintf(os.Stderr, "Error: failed to load scenario '%s'\n", scenarioPath)
		return 1
	}

	s.scenario = meta
	s.duration = meta.DurationUS
	pcg32Seed(&s.rng, meta.Seed)

	fmt.Fprintf(os.Stderr, "Loaded scenario '%s'\n", meta.Name)
	fmt.Fprintf(os.Stderr, "  Duration: %d ms\n", meta.DurationUS/1000)
	fmt.Fprintf(os.Stderr, "  Nodes: %d\n", nodeCount(&s.nodes))
	fmt.Fprintf(os.Stderr, "  Events: %d\n", eventQueueCount(&s.events))
	fmt.Fprintf(os.Stderr, "  Mode: %s\n", func() string {
		if meta.Deterministic { return "deterministic" }
		return "stochastic"
	}())
	fmt.Fprintf(os.Stderr, "  Seed: %d\n\n", meta.Seed)

	// Emit initial nodes
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && node.active {
			C.emit_node_joined(s.emitFile, 0, &node.id[0], node.addr, node.x, node.y)
		}
	}
	fmt.Fprintf(io.Writer(s.pipeW), "{\"type\":\"config\",\"timestamp_us\":0,\"radio_range\":%.2f}\n",
		float32(s.radio._range))

	// Schedule initial ticks
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node == nil || !node.active { continue }
		var tickEvt C.sim_event_t
		cID := C.CString(C.GoString(&node.id[0]))
		C.bridge_make_tick_event(&tickEvt, C.uint64_t(uint64(i)*100000), cID, 0)
		C.free(unsafe.Pointer(cID))
		eventQueuePush(&s.events, &tickEvt)
	}

	// Process all events
	var evt C.sim_event_t
	for eventQueuePop(&s.events, &evt) {
		ts := uint64(evt.timestamp_us)
		if ts > s.duration { break }
		s.simTime = ts
		setSimTime(s.simTime)
		s.dispatchEvent(&evt)
	}

	// Final metrics
	s.complete()

	fmt.Fprintf(os.Stderr, "\nSimulation complete.\n")
	return 0
}
```

**Step 2: Verify compilation**

Run: `cd simulator/gosim && go build -o bramble-gosim . 2>&1`
Expected: Compiles successfully.

**Step 3: Commit**

```bash
git add simulator/gosim/sim.go
git commit -m "feat(sim): implement Go simulation engine with state machine"
```

---

## Phase 3: WebSocket Hub & HTTP Server

### Task 5: Create WebSocket hub

**Files:**
- Create: `simulator/gosim/ws.go`

**Step 1: Add gorilla/websocket dependency**

Run: `cd simulator/gosim && go get github.com/gorilla/websocket`

**Step 2: Create `ws.go`**

```go
// simulator/gosim/ws.go
package main

import (
	"encoding/json"
	"log"
	"net/http"
	"sync"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

type Client struct {
	conn *websocket.Conn
	send chan []byte
}

type Hub struct {
	mu         sync.RWMutex
	clients    map[*Client]bool
	sim        *Sim
}

func NewHub(sim *Sim) *Hub {
	return &Hub{
		clients: make(map[*Client]bool),
		sim:     sim,
	}
}

func (h *Hub) Broadcast(msg []byte) {
	h.mu.RLock()
	defer h.mu.RUnlock()

	for client := range h.clients {
		select {
		case client.send <- msg:
		default:
			// Client too slow, drop message
		}
	}
}

func (h *Hub) HandleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println("[ws] Upgrade error:", err)
		return
	}

	client := &Client{
		conn: conn,
		send: make(chan []byte, 256),
	}

	h.mu.Lock()
	h.clients[client] = true
	h.mu.Unlock()

	log.Println("[ws] Client connected")

	// Writer goroutine
	go func() {
		defer conn.Close()
		for msg := range client.send {
			if err := conn.WriteMessage(websocket.TextMessage, msg); err != nil {
				break
			}
		}
	}()

	// Reader goroutine (processes commands from client)
	go func() {
		defer func() {
			h.mu.Lock()
			delete(h.clients, client)
			h.mu.Unlock()
			close(client.send)
			conn.Close()
			log.Println("[ws] Client disconnected")
		}()

		for {
			_, message, err := conn.ReadMessage()
			if err != nil {
				break
			}

			var cmd Command
			if err := json.Unmarshal(message, &cmd); err != nil {
				continue
			}
			h.sim.SendCommand(cmd)
		}
	}()

	// Auto-start default scenario if sim is idle
	if h.sim.GetState() == StateIdle {
		h.sim.SendCommand(Command{Type: "start", Scenario: "10-node-grid"})
	}
}
```

**Step 3: Verify compilation**

Run: `cd simulator/gosim && go build -o bramble-gosim . 2>&1`
Expected: Compiles.

**Step 4: Commit**

```bash
git add simulator/gosim/ws.go simulator/gosim/go.mod simulator/gosim/go.sum
git commit -m "feat(sim): add WebSocket hub for client connections"
```

---

### Task 6: Create HTTP server and main entry point

**Files:**
- Modify: `simulator/gosim/main.go`

**Step 1: Replace placeholder `main.go`**

```go
// simulator/gosim/main.go
package main

import (
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	// CLI flags
	port := flag.Int("port", 3000, "HTTP server port")
	uiDir := flag.String("ui", "", "Path to UI static files directory")
	scenarioDir := flag.String("scenarios", "", "Path to scenarios directory")
	headless := flag.Bool("headless", false, "Run headless (no server, output to stdout)")
	scenario := flag.String("scenario", "", "Scenario file path (headless mode)")
	flag.Parse()

	// Defaults
	if *scenarioDir == "" {
		// Try relative paths
		candidates := []string{"../scenarios", "scenarios", "/scenarios"}
		for _, c := range candidates {
			if info, err := os.Stat(c); err == nil && info.IsDir() {
				*scenarioDir = c
				break
			}
		}
		if *scenarioDir == "" {
			log.Fatal("Cannot find scenarios directory. Use --scenarios flag.")
		}
	}

	if *headless {
		scenPath := *scenario
		if scenPath == "" && flag.NArg() > 0 {
			scenPath = flag.Arg(0)
		}
		if scenPath == "" {
			fmt.Fprintf(os.Stderr, "Usage: bramble-gosim --headless --scenario <path>\n")
			os.Exit(1)
		}

		sim := NewSim(*scenarioDir, func(msg []byte) {
			// In headless mode, broadcast writes to stdout
			os.Stdout.Write(msg)
			os.Stdout.Write([]byte("\n"))
		}, true)

		// Start pipe reader
		go sim.readPipe()

		code := sim.RunHeadless(scenPath)
		sim.Stop()
		os.Exit(code)
	}

	// Server mode
	if *uiDir == "" {
		candidates := []string{"../ui/dist", "ui/dist", "/ui"}
		for _, c := range candidates {
			if info, err := os.Stat(c); err == nil && info.IsDir() {
				*uiDir = c
				break
			}
		}
	}

	// Create sim + hub
	var hub *Hub
	sim := NewSim(*scenarioDir, func(msg []byte) {
		if hub != nil {
			hub.Broadcast(msg)
		}
	}, false)
	hub = NewHub(sim)
	sim.Start()

	// HTTP routes
	mux := http.NewServeMux()

	// WebSocket endpoint
	mux.HandleFunc("/ws", hub.HandleWS)
	// Also handle bare upgrade at root path for backward compat
	// (current UI connects to ws://host:port without /ws path)

	// REST: list scenarios
	mux.HandleFunc("/api/scenarios", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			entries, err := os.ReadDir(*scenarioDir)
			if err != nil {
				http.Error(w, err.Error(), 500)
				return
			}
			var names []string
			for _, e := range entries {
				if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
					names = append(names, strings.TrimSuffix(e.Name(), ".json"))
				}
			}
			w.Header().Set("Content-Type", "application/json")
			fmt.Fprintf(w, "[")
			for i, name := range names {
				if i > 0 { fmt.Fprintf(w, ",") }
				fmt.Fprintf(w, "%q", name)
			}
			fmt.Fprintf(w, "]")
		}
	})

	// REST: upload scenario
	mux.HandleFunc("/api/scenarios/upload", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" {
			http.Error(w, "Method not allowed", 405)
			return
		}

		r.ParseMultipartForm(10 << 20) // 10MB max
		file, header, err := r.FormFile("file")
		if err != nil {
			// Try raw body
			defer r.Body.Close()
			body := make([]byte, 10<<20)
			n, _ := r.Body.Read(body)
			body = body[:n]
			name := fmt.Sprintf("uploaded-%d", os.Getpid())
			destPath := filepath.Join(*scenarioDir, name+".json")
			os.WriteFile(destPath, body, 0644)
			w.Header().Set("Content-Type", "application/json")
			fmt.Fprintf(w, `{"name":"%s"}`, name)
			return
		}
		defer file.Close()

		name := strings.TrimSuffix(header.Filename, ".json")
		destPath := filepath.Join(*scenarioDir, name+".json")

		dest, err := os.Create(destPath)
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		defer dest.Close()

		buf := make([]byte, header.Size)
		file.Read(buf)
		dest.Write(buf)

		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"name":"%s"}`, name)
	})

	// Static files (UI)
	if *uiDir != "" {
		fileServer := http.FileServer(http.Dir(*uiDir))
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			// WebSocket upgrade check: if this is a WS upgrade on /,
			// route to WS handler (backward compat with current UI)
			if r.Header.Get("Upgrade") == "websocket" {
				hub.HandleWS(w, r)
				return
			}

			// Try to serve the file
			path := filepath.Join(*uiDir, r.URL.Path)
			if _, err := fs.Stat(os.DirFS(*uiDir), strings.TrimPrefix(r.URL.Path, "/")); err != nil {
				// SPA fallback
				http.ServeFile(w, r, filepath.Join(*uiDir, "index.html"))
				return
			}
			_ = path
			fileServer.ServeHTTP(w, r)
		})
	}

	addr := fmt.Sprintf("0.0.0.0:%d", *port)
	log.Printf("[server] Listening on http://%s", addr)
	log.Printf("[server] UI: %s", *uiDir)
	log.Printf("[server] Scenarios: %s", *scenarioDir)

	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatal(err)
	}
}
```

**Step 2: Verify compilation and run**

Run: `cd simulator/gosim && go build -o bramble-gosim . 2>&1`
Expected: Compiles successfully.

Run: `cd simulator/gosim && ./bramble-gosim --help`
Expected: Shows flag help.

**Step 3: Commit**

```bash
git add simulator/gosim/main.go
git commit -m "feat(sim): add HTTP server with static files, REST API, and WebSocket"
```

---

## Phase 4: Build & Deploy

### Task 7: Fix compilation issues and test cgo build

At this point, there will almost certainly be compilation issues. cgo is finicky with unions, `bool` types, struct field access, and include paths. This task is dedicated to fixing whatever breaks.

**Step 1: Attempt full build**

Run: `cd simulator/gosim && go build -v -o bramble-gosim . 2>&1`

**Step 2: Fix issues iteratively**

Common issues to expect and fix:
- `bool` type conflicts between C99 `_Bool` and Go — use `int` in bridge.h instead
- Union access in cgo — verify all union fields go through bridge accessors
- `C.true` / `C.false` — use manual comparison instead
- Struct field name conflicts with Go keywords (e.g., `range` → access via `_range`)
- Missing includes or include path issues
- `FILE*` handling across cgo boundary
- Linking errors from missing symbols

**Step 3: Test with a scenario**

Run: `cd simulator/gosim && ./bramble-gosim --headless --scenario ../scenarios/test-2-node.json 2>stderr.log | head -50`
Expected: JSON event lines on stdout, scenario info on stderr.

**Step 4: Compare output with original C engine**

Run: `cd simulator && docker compose exec bramble-sim /app/engine/bramble-sim /app/scenarios/test-2-node.json 2>/dev/null | wc -l` (or use existing headless runner)
Compare event count and final metrics between old and new engine.

**Step 5: Commit**

```bash
git add -A simulator/gosim/
git commit -m "fix(sim): resolve cgo compilation issues"
```

---

### Task 8: Update Dockerfile for Go build

**Files:**
- Create: `simulator/Dockerfile.go` (new, alongside existing Dockerfile)
- Modify: `simulator/docker-compose.yml`

**Step 1: Create `Dockerfile.go`**

```dockerfile
# Stage 1: Build React UI
FROM node:22-slim AS ui-build
WORKDIR /app/simulator/ui
COPY simulator/ui/package*.json .
RUN npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 2: Build Go server (with embedded C via cgo)
FROM golang:1.24-bookworm AS go-build
RUN apt-get update && apt-get install -y gcc libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY components/ components/
COPY test/stubs/ test/stubs/
COPY simulator/engine/ simulator/engine/
COPY simulator/gosim/ simulator/gosim/
WORKDIR /app/simulator/gosim
RUN CGO_ENABLED=1 go build -o /bramble-gosim .

# Stage 3: Runtime (minimal)
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=go-build /bramble-gosim /usr/local/bin/bramble-gosim
COPY --from=ui-build /app/simulator/ui/dist /ui
COPY simulator/scenarios /scenarios
EXPOSE 3000
CMD ["bramble-gosim", "--ui", "/ui", "--scenarios", "/scenarios"]
```

**Step 2: Update `docker-compose.yml`**

```yaml
services:
  bramble-sim:
    build:
      context: ..
      dockerfile: simulator/Dockerfile.go
    ports:
      - "3003:3000"
    volumes:
      - ./scenarios:/scenarios:ro
```

**Step 3: Build and test**

Run: `cd /home/justin/.openclaw/workspace/bramble && docker compose -f simulator/docker-compose.yml build 2>&1 | tail -20`
Expected: Builds successfully.

Run: `cd /home/justin/.openclaw/workspace/bramble && docker compose -f simulator/docker-compose.yml up -d && sleep 3 && curl -s http://localhost:3003/api/scenarios | head`
Expected: JSON array of scenario names.

**Step 4: Commit**

```bash
git add simulator/Dockerfile.go simulator/docker-compose.yml
git commit -m "feat(sim): add Docker build for Go simulation server"
```

---

### Task 9: Update headless runner script

**Files:**
- Modify: `simulator/scripts/run-scenario.sh`

**Step 1: Update the script to use the Go binary**

The script should detect whether it's running inside Docker (Go binary at `/usr/local/bin/bramble-gosim`) or locally (`simulator/gosim/bramble-gosim`). Update the engine invocation to use `--headless --scenario`.

Read the existing script first: `cat simulator/scripts/run-scenario.sh`

Then update the `ENGINE_BIN` detection and invocation to call:
```bash
bramble-gosim --headless --scenario "$SCENARIO_PATH"
```

instead of the old direct C binary invocation.

**Step 2: Test headless runner**

Run: `cd /home/justin/.openclaw/workspace/bramble && bash simulator/scripts/run-scenario.sh test-2-node`
Expected: Same output format as before.

**Step 3: Commit**

```bash
git add simulator/scripts/run-scenario.sh
git commit -m "feat(sim): update headless runner for Go engine"
```

---

## Phase 5: End-to-End Verification

### Task 10: Verify WebSocket simulation in browser

**Step 1: Start the container**

Run: `cd /home/justin/.openclaw/workspace/bramble && docker compose -f simulator/docker-compose.yml up -d`

**Step 2: Open in browser and verify**

Navigate to `http://192.168.6.35:3003` (or Tailscale IP).
Verify:
- Nodes appear on canvas
- "Ready" state shown
- Play button starts simulation
- Metrics update in dashboard
- Event log populates
- Speed control works
- Scenario loader lists and loads scenarios
- Packet animations work

**Step 3: Test interactive commands**

- Click "Add Node" — verify new node appears AND participates (beacons, routes form)
- Change speed mid-simulation
- Pause and resume
- Load a different scenario

**Step 4: Run all scenarios headless and compare metrics**

Run each scenario through both old engine (if still available) and new Go engine. Compare delivery rates and packet counts.

```bash
for s in test-2-node 3-node-linear 10-node-grid ideal-10-node ideal-massive; do
  echo "=== $s ==="
  bash simulator/scripts/run-scenario.sh "$s" 2>/dev/null | tail -1
done
```

Expected: Same or very similar metrics as the original engine.

**Step 5: Commit any fixes**

```bash
git add -A
git commit -m "fix(sim): end-to-end verification fixes"
```

---

### Task 11: Clean up old Node.js relay and C main.c

**Files:**
- Delete: `simulator/server/relay.ts`
- Delete: `simulator/server/package.json`
- Delete: `simulator/server/package-lock.json`
- Delete: `simulator/server/tsconfig.json` (if exists)
- Delete: `simulator/engine/main.c`
- Delete: `simulator/Dockerfile` (old)
- Rename: `simulator/Dockerfile.go` → `simulator/Dockerfile`

**Step 1: Remove old files**

```bash
cd /home/justin/.openclaw/workspace/bramble
rm -rf simulator/server/
rm simulator/engine/main.c
mv simulator/Dockerfile simulator/Dockerfile.old
mv simulator/Dockerfile.go simulator/Dockerfile
```

**Step 2: Verify build still works**

Run: `docker compose -f simulator/docker-compose.yml build 2>&1 | tail -5`
Expected: Builds successfully.

**Step 3: Commit**

```bash
git add -A
git commit -m "chore(sim): remove old Node.js relay and C main.c"
```

---

### Task 12: Update documentation

**Files:**
- Modify: `simulator/README.md`

**Step 1: Update README**

Update the README to reflect the new architecture:
- Go + C binary replaces Node.js relay + C engine
- New CLI flags (`--headless`, `--scenario`, `--port`, `--ui`, `--scenarios`)
- Interactive commands (add_node, send_message, etc.)
- Docker build instructions

**Step 2: Commit**

```bash
git add simulator/README.md
git commit -m "docs(sim): update README for Go simulation server"
```

---

## Summary

| Phase | Tasks | What it builds |
|-------|-------|----------------|
| 1: C Bridge | Tasks 1–3 | Go module, C compilation unit, cgo bindings |
| 2: Sim Engine | Task 4 | State machine, event loop, command processing |
| 3: Server | Tasks 5–6 | WebSocket hub, HTTP server, CLI entry point |
| 4: Build | Tasks 7–9 | Fix compilation, Docker, headless runner |
| 5: Verify | Tasks 10–12 | E2E testing, cleanup, docs |

**Total: 12 tasks, ~30-40 bite-sized steps**

**Key risk:** Task 7 (fixing cgo compilation) is the wild card. cgo is notoriously picky about types, unions, and cross-language struct access. Budget extra time there. The bridge.c/bridge.h layer is specifically designed to minimize this pain by keeping complex C logic in C and giving Go clean function signatures.
