# Bramble Mesh Simulator Phase 2: Core Simulation Logic

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Transform the topology viewer into a real network simulator with autonomous nodes, packet routing, playback controls, and animated visualization.

**Architecture:** Per-node tick loops calling actual Bramble routing/forwarding/discovery code, with server-side event buffering for playback control.

**Tech Stack:** C (gcc), existing Bramble components, Node.js/TypeScript, React/SVG

---

## Overview

The Phase 1 simulator handles topology events (join/leave/move) but the `EVT_GENERATE_MESSAGE` handler is a stub. Phase 2 wires in the full Bramble routing stack, makes nodes autonomous, adds playback controls, and animates packets on the canvas.

**Key insight from `test_integration.c`:** The routing stack is entirely synchronous and stateless with respect to the caller. Each node owns its `routing_table_t`, `neighbor_table_t`, `reverse_route_table_t`, `rreq_dedup_t`, and `pending_discovery_table_t`. The simulator drives them by calling the same functions the firmware would call.

---

## Section 1: Node Autonomy — Per-Node Tick Loop

### Background

Right now `main.c` handles only scripted events from the scenario file. Real ESP32 firmware runs periodic FreeRTOS tasks: beacon TX every ~30s, neighbor purge every minute, route expiry maintenance, and discovery retry timers. We replicate this with a `EVT_TICK_NODE` event that each active node schedules for itself.

### Task 1.1 — Add tick event type and per-node tick constants

**File:** `bramble/simulator/engine/sim_event.h`

Add `EVT_TICK_NODE` to the event type enum and a new `tick_event_data_t` union member:

```c
// In event_type_t enum, after EVT_METRICS_TICK:
EVT_TICK_NODE,

// New data struct:
typedef struct {
    char node_id[16];
    uint32_t tick_seq;   /* monotonically increasing per-node */
} tick_event_data_t;

// In sim_event_t data union:
tick_event_data_t tick;
```

**File:** `bramble/simulator/engine/sim_node.h`

Add tick configuration constants and per-node tick state to `sim_node_t`:

```c
/* Tick intervals (microseconds) */
#define NODE_BEACON_INTERVAL_US     30000000ULL   /* 30 s */
#define NODE_NEIGHBOR_PURGE_US      60000000ULL   /* 60 s */
#define NODE_ROUTE_MAINT_US         60000000ULL   /* 60 s */
#define NODE_DISCOVERY_CHECK_US      5000000ULL   /*  5 s */
#define NODE_TICK_INTERVAL_US        1000000ULL   /*  1 s base tick */

/* Add to sim_node_t: */
uint32_t tick_seq;
uint64_t last_beacon_us;
uint64_t last_neighbor_purge_us;
uint64_t last_route_maint_us;
uint64_t last_discovery_check_us;
uint32_t uptime_min;     /* incremented each tick for beacon */

/* Add to sim_node_t stats: */
uint64_t beacons_sent;
```

**Verify:** `make -C bramble/simulator/engine` compiles cleanly.

---

### Task 1.2 — Schedule initial tick events at scenario load

**File:** `bramble/simulator/engine/main.c`

After the initial `emit_node_joined` loop, schedule the first tick for each active node. Stagger by node index × 100 ms to avoid tick storms:

```c
/* Schedule initial node ticks (staggered) */
for (int i = 0; i < g_nodes.count; i++) {
    sim_node_t *node = &g_nodes.nodes[i];
    if (!node->active) continue;
    sim_event_t tick_evt = {0};
    tick_evt.type = EVT_TICK_NODE;
    tick_evt.timestamp_us = (uint64_t)(i) * 100000ULL;  /* 100 ms stagger */
    strncpy(tick_evt.data.tick.node_id, node->id, NODE_ID_LEN - 1);
    tick_evt.data.tick.tick_seq = 0;
    event_queue_push(&g_events, &tick_evt);
}
```

**Verify:** Build passes. Run with `./bramble-sim scenarios/3-node-linear.json 2>&1 | head -5` — no crash.

---

### Task 1.3 — Implement `node_tick()` in sim_node.c

**File:** `bramble/simulator/engine/sim_node.c`

Add a new function that performs all periodic node maintenance. It uses the current simulation time (passed in) to decide what to do, and returns a list of outbound packets to be radio-transmitted by the caller:

```c
/* In sim_node.h — add the output packet type */
#define NODE_TICK_MAX_OUTBOUND 4

typedef struct {
    uint8_t  data[256];
    uint16_t len;
    bool     is_broadcast;   /* false = unicast to next_hop */
    uint32_t dest_addr;      /* ignored if broadcast */
    uint8_t  pkt_type;       /* PKT_TYPE_* for emitter */
} outbound_packet_t;

typedef struct {
    outbound_packet_t pkts[NODE_TICK_MAX_OUTBOUND];
    int count;
} node_tick_result_t;

void node_tick(sim_node_t *node, uint64_t now_us, node_tick_result_t *result);
```

**Implementation in sim_node.c:**

```c
#include "../../components/routing/include/beacon.h"
#include "../../components/routing/include/discovery.h"

void node_tick(sim_node_t *node, uint64_t now_us, node_tick_result_t *result) {
    result->count = 0;
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    /* 1. Beacon transmission */
    if (now_us - node->last_beacon_us >= NODE_BEACON_INTERVAL_US) {
        node->last_beacon_us = now_us;
        node->uptime_min++;

        bramble_beacon_t beacon = beacon_build(
            node->addr,
            node->addr,               /* pubkey_hash = addr (sim simplification) */
            node->uptime_min,
            100,                      /* battery_pct: always full in sim */
            0,                        /* tx_queue_depth */
            (uint8_t)neighbor_count(&node->neighbors),
            0,                        /* flags */
            now_ms,                   /* network_time */
            0                         /* time_confidence */
        );

        outbound_packet_t *out = &result->pkts[result->count++];
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
        for (int i = 0; i < node->pending_discoveries.count && result->count < NODE_TICK_MAX_OUTBOUND; i++) {
            pending_discovery_t *d = &node->pending_discoveries.entries[i];
            if (discovery_should_retry(d, now_ms)) {
                /* Re-originate RREQ */
                uint32_t query_id = d->query_id;
                bramble_rreq_t rreq = rreq_build_originator(
                    node->addr, d->dest_addr, query_id, node->addr);
                discovery_record_attempt(d, now_ms);

                outbound_packet_t *out = &result->pkts[result->count++];
                bramble_rreq_serialize(&rreq, out->data, RREQ_SIZE);
                out->len = RREQ_SIZE;
                out->is_broadcast = true;
                out->dest_addr = 0xFFFFFFFF;
                out->pkt_type = PKT_TYPE_RREQ;
            }
        }
    }
}
```

**Verify:**
```bash
make -C bramble/simulator/engine
# Expected: compiles cleanly, no undefined reference errors
```

---

### Task 1.4 — Handle `EVT_TICK_NODE` in main.c and reschedule

**File:** `bramble/simulator/engine/main.c`

Add a case to `handle_event()`. After running the tick, reschedule the next tick and radio-transmit any outbound packets:

```c
case EVT_TICK_NODE: {
    sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.tick.node_id);
    if (!node || !node->active) break;

    node_tick_result_t tick_result;
    node_tick(node, event->timestamp_us, &tick_result);

    /* Transmit outbound packets via radio model */
    for (int i = 0; i < tick_result.count; i++) {
        outbound_packet_t *pkt = &tick_result.pkts[i];
        sim_radio_broadcast(node, pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, event->timestamp_us);
    }

    /* Reschedule tick */
    sim_event_t next = {0};
    next.type = EVT_TICK_NODE;
    next.timestamp_us = event->timestamp_us + NODE_TICK_INTERVAL_US;
    strncpy(next.data.tick.node_id, node->id, NODE_ID_LEN - 1);
    next.data.tick.tick_seq = event->data.tick.tick_seq + 1;
    event_queue_push(&g_events, &next);
    break;
}
```

**Verify:**
```bash
make -C bramble/simulator/engine
./bramble-sim scenarios/3-node-linear.json 2>&1 | grep -c '"type":"node_joined"'
# Expected: 3
```

---

## Section 2: Radio Transmission Helper

### Task 2.1 — Add `sim_radio_broadcast()` to sim_radio.c

This is the core function that takes a packet from a transmitting node, iterates all other nodes, calls `radio_can_receive()` for each, and schedules `EVT_RECEIVE_PACKET` events with propagation delay. It also emits `packet_sent` and handles the metrics bookkeeping.

**File:** `bramble/simulator/engine/sim_radio.h` — add declaration:

```c
#include "sim_event.h"
#include "sim_emitter.h"
#include "sim_metrics.h"

/* Packet types for the outbound_packet_t (forward decl needed) */
#include "sim_node.h"  /* already included */

void sim_radio_broadcast(
    sim_node_t *tx_node,
    const outbound_packet_t *pkt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    uint64_t now_us
);
```

**File:** `bramble/simulator/engine/sim_radio.c` — add implementation:

```c
void sim_radio_broadcast(
    sim_node_t *tx_node,
    const outbound_packet_t *pkt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    uint64_t now_us)
{
    /* Emit packet_sent event for visualization */
    emit_packet_sent_typed(stdout, now_us, tx_node->id, tx_node->addr,
                           pkt->dest_addr, pkt->len, pkt->pkt_type);
    metrics_record_packet_sent(metrics);
    tx_node->packets_sent++;

    /* Deliver to all nodes in range */
    for (int i = 0; i < nodes->count; i++) {
        sim_node_t *rx = &nodes->nodes[i];
        if (rx == tx_node || !rx->active) continue;

        /* For unicast, skip non-target nodes */
        if (!pkt->is_broadcast && pkt->dest_addr != rx->addr) continue;

        if (!radio_can_receive(radio, tx_node, rx, rng)) {
            emit_packet_dropped(stdout, now_us, rx->id, "radio_loss");
            metrics_record_packet_dropped(metrics);
            continue;
        }

        float dist = radio_distance(tx_node, rx);
        uint64_t delay_us = radio_propagation_delay_us(radio, dist);
        int8_t rssi = radio_compute_rssi(radio, dist);

        /* Schedule receive event */
        sim_event_t recv_evt = {0};
        recv_evt.type = EVT_RECEIVE_PACKET;
        recv_evt.timestamp_us = now_us + delay_us;
        recv_evt.data.packet.src_addr = tx_node->addr;
        recv_evt.data.packet.dest_addr = rx->addr;
        recv_evt.data.packet.len = pkt->len;
        memcpy(recv_evt.data.packet.data, pkt->data, pkt->len);
        /* Stash rssi in unused byte (event data has room) */
        recv_evt.data.packet.rssi = rssi;
        event_queue_push(events, &recv_evt);
    }
}
```

> **Note:** `packet_event_data_t` needs an `int8_t rssi` field added — add it in `sim_event.h` alongside `src_addr/dest_addr/data/len`.

**Verify:** `make -C bramble/simulator/engine` — clean compile.

---

### Task 2.2 — Add `rssi` field and `emit_packet_sent_typed()` 

**File:** `bramble/simulator/engine/sim_event.h` — add `rssi` to `packet_event_data_t`:

```c
typedef struct {
    uint32_t src_addr;
    uint32_t dest_addr;
    int8_t   rssi;      /* ← add this */
    uint8_t  data[256];
    uint16_t len;
} packet_event_data_t;
```

**File:** `bramble/simulator/engine/sim_emitter.h` — add the typed variant:

```c
/* packet_type_str: "RREQ", "RREP", "RERR", "BEACON", "DATA" */
void emit_packet_sent_typed(FILE *out, uint64_t timestamp_us,
    const char *node_id, uint32_t src_addr, uint32_t dest_addr,
    uint16_t size, uint8_t pkt_type);

void emit_packet_received_typed(FILE *out, uint64_t timestamp_us,
    const char *node_id, uint32_t src_addr, int8_t rssi,
    uint16_t size, uint8_t pkt_type);
```

**File:** `bramble/simulator/engine/sim_emitter.c` — implement both:

```c
static const char *pkt_type_name(uint8_t t) {
    switch (t) {
        case PKT_TYPE_RREQ:   return "RREQ";
        case PKT_TYPE_RREP:   return "RREP";
        case PKT_TYPE_RERR:   return "RERR";
        case PKT_TYPE_BEACON: return "BEACON";
        case PKT_TYPE_DATA:   return "DATA";
        default:              return "UNKNOWN";
    }
}

void emit_packet_sent_typed(FILE *out, uint64_t timestamp_us,
    const char *node_id, uint32_t src_addr, uint32_t dest_addr,
    uint16_t size, uint8_t pkt_type)
{
    fprintf(out,
        "{\"type\":\"packet_sent\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"src\":\"0x%08X\",\"dest\":\"0x%08X\""
        ",\"pkt_type\":\"%s\",\"size\":%u}\n",
        (unsigned long long)timestamp_us, node_id,
        src_addr, dest_addr, pkt_type_name(pkt_type), size);
    fflush(out);
}

void emit_packet_received_typed(FILE *out, uint64_t timestamp_us,
    const char *node_id, uint32_t src_addr, int8_t rssi,
    uint16_t size, uint8_t pkt_type)
{
    fprintf(out,
        "{\"type\":\"packet_received\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"src\":\"0x%08X\""
        ",\"pkt_type\":\"%s\",\"rssi\":%d,\"size\":%u}\n",
        (unsigned long long)timestamp_us, node_id,
        src_addr, pkt_type_name(pkt_type), rssi, size);
    fflush(out);
}
```

**Verify:** `make -C bramble/simulator/engine` — clean compile.

**Commit:** `git -C bramble commit -am "sim: node tick loop + radio broadcast helper"`

---

## Section 3: Packet Forwarding Pipeline

### Task 3.1 — Handle `EVT_RECEIVE_PACKET` in main.c

This is the core routing dispatcher. When a node receives a packet, deserialize the header and dispatch to the right handler based on `type` field.

**File:** `bramble/simulator/engine/main.c` — add to `handle_event()`:

```c
case EVT_RECEIVE_PACKET: {
    sim_node_t *rx = node_array_find_by_addr(&g_nodes, event->data.packet.dest_addr);
    /* For broadcast packets, dest_addr is 0xFFFFFFFF — find by matching scheduled event */
    /* Actually: broadcast already delivered individually per node in sim_radio_broadcast */
    /* So dest_addr here is always the specific rx node's address */
    if (!rx || !rx->active) break;

    uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);
    uint8_t *buf = event->data.packet.data;
    uint16_t len  = event->data.packet.len;
    int8_t   rssi = event->data.packet.rssi;

    /* Deserialize header to determine packet type */
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) break;

    emit_packet_received_typed(stdout, event->timestamp_us, rx->id,
                               event->data.packet.src_addr, rssi, len, hdr.type);
    rx->packets_received++;
    metrics_record_packet_delivered(&g_metrics, 0); /* latency tracked separately */

    switch (hdr.type) {
        case PKT_TYPE_BEACON:
            handle_beacon_received(rx, buf, len, rssi, event->timestamp_us, now_ms);
            break;
        case PKT_TYPE_RREQ:
            handle_rreq_received(rx, buf, len, rssi, event->timestamp_us, now_ms);
            break;
        case PKT_TYPE_RREP:
            handle_rrep_received(rx, buf, len, event->timestamp_us, now_ms);
            break;
        case PKT_TYPE_RERR:
            handle_rerr_received(rx, buf, len, event->timestamp_us, now_ms);
            break;
        case PKT_TYPE_DATA:
            handle_data_received(rx, buf, len, event->timestamp_us, now_ms);
            break;
        default:
            break;
    }
    break;
}
```

---

### Task 3.2 — Implement beacon receive handler

**File:** `bramble/simulator/engine/main.c` — add before `handle_event()`:

```c
static void handle_beacon_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                   int8_t rssi, uint64_t now_us, uint32_t now_ms)
{
    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, buf, len) != ESP_OK) return;

    /* Update neighbor table. snr=0 (not modeled at this layer). */
    int updated = neighbor_update(&rx->neighbors, beacon.src_addr,
                                  rssi, 0, beacon.pubkey_hash, now_ms);
    if (updated >= 0) {
        /* Emit route_added if this is a new neighbor we can route to directly */
        route_entry_t *existing = route_lookup(&rx->routes, beacon.src_addr);
        if (!existing || existing->state == ROUTE_BROKEN) {
            route_install(&rx->routes, beacon.src_addr, beacon.src_addr,
                          1, (uint8_t)(100 + rssi), /* metric: base+rssi */
                          ROUTE_ACTIVE, now_ms);
            emit_route_added(stdout, now_us, rx->id, beacon.src_addr,
                             beacon.src_addr, 1);
            anomaly_check_route_flap(&g_flap_tracker[rx - g_nodes.nodes],
                                     beacon.src_addr, beacon.src_addr,
                                     now_us, stdout, rx->id);
        }
    }
    (void)now_us;
}
```

> **Note:** `g_flap_tracker` is a per-node anomaly tracker array — see Task 6.1.

---

### Task 3.3 — Implement RREQ receive handler

Based on `test_integration.c` steps 2–3: dedup check → reverse route store → forward or answer.

**File:** `bramble/simulator/engine/main.c`:

```c
static void handle_rreq_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  int8_t rssi, uint64_t now_us, uint32_t now_ms)
{
    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, buf, len) != ESP_OK) return;

    /* Dedup: drop if already seen this query */
    if (rreq_dedup_check_and_add(&rx->rreq_dedup, rreq.query_id, now_ms)) return;

    /* Store reverse route for RREP to follow back */
    reverse_route_add(&rx->reverse_routes, rreq.query_id, rreq.prev_hop, now_ms);

    if (rreq.header.dest_addr == rx->addr) {
        /* We are the destination — build RREP */
        bramble_rrep_t rrep = rrep_build_destination(&rreq, rx->addr);

        outbound_packet_t pkt = {0};
        bramble_rrep_serialize(&rrep, pkt.data, RREP_SIZE);
        pkt.len = RREP_SIZE;
        pkt.is_broadcast = false;
        pkt.dest_addr = rreq.prev_hop;  /* unicast toward originator */
        pkt.pkt_type = PKT_TYPE_RREP;

        sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, now_us);
    } else if (rreq.header.hop_limit > 1) {
        /* Forward RREQ */
        int8_t snr = 0; /* SNR not modeled */
        bramble_rreq_t fwd = rreq_forward(&rreq, rx->addr, rssi, snr);

        outbound_packet_t pkt = {0};
        bramble_rreq_serialize(&fwd, pkt.data, RREQ_SIZE);
        pkt.len = RREQ_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr = 0xFFFFFFFF;
        pkt.pkt_type = PKT_TYPE_RREQ;
        rx->packets_forwarded++;

        sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, now_us);
    }
}
```

---

### Task 3.4 — Implement RREP receive handler

Based on `test_integration.c` steps 4–5: install forward route → forward RREP toward originator.

**File:** `bramble/simulator/engine/main.c`:

```c
static void handle_rrep_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  uint64_t now_us, uint32_t now_ms)
{
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, buf, len) != ESP_OK) return;

    /* Install forward route to RREP source */
    route_install(&rx->routes, rrep.src_addr, rrep.next_hop,
                  rrep.hop_count, rrep.route_metric, ROUTE_ACTIVE, now_ms);
    emit_route_added(stdout, now_us, rx->id, rrep.src_addr, rrep.next_hop, rrep.hop_count);

    /* Are we the original RREQ sender? Check pending discoveries */
    pending_discovery_t *pd = discovery_lookup_by_query(&rx->pending_discoveries, rrep.query_id);
    if (pd) {
        /* Route acquired — flush any queued data packets */
        discovery_remove(&rx->pending_discoveries, pd->dest_addr);
        /* (Data packets queued during discovery would be flushed here in a full impl) */
        return;
    }

    /* Not the originator — forward RREP back toward originator */
    reverse_route_t *rr = reverse_route_lookup(&rx->reverse_routes, rrep.query_id);
    if (!rr) return;  /* Lost reverse route — drop */

    bramble_rrep_t fwd = rrep_forward(&rrep, rr->prev_hop);
    outbound_packet_t pkt = {0};
    bramble_rrep_serialize(&fwd, pkt.data, RREP_SIZE);
    pkt.len = RREP_SIZE;
    pkt.is_broadcast = false;
    pkt.dest_addr = rr->prev_hop;
    pkt.pkt_type = PKT_TYPE_RREP;
    rx->packets_forwarded++;

    sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                        &g_events, &g_metrics, now_us);
}
```

---

### Task 3.5 — Implement RERR and DATA receive handlers

**File:** `bramble/simulator/engine/main.c`:

```c
static void handle_rerr_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  uint64_t now_us, uint32_t now_ms)
{
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, buf, len) != ESP_OK) return;
    rerr_handle(&rx->routes, &rerr);
    emit_link_broken(stdout, now_us, rx->id, rerr.broken_next_hop);
    (void)now_ms;
}

/* Per-message tracking for latency */
typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint64_t sent_us;
    bool     active;
} msg_tracker_t;

#define MAX_MSG_TRACK 256
static msg_tracker_t g_msg_track[MAX_MSG_TRACK];

static void handle_data_received(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                                  uint64_t now_us, uint32_t now_ms)
{
    bramble_header_t hdr;
    if (bramble_header_deserialize(&hdr, buf, len) != ESP_OK) return;

    if (hdr.dest_addr == rx->addr) {
        /* Final destination — record delivery latency */
        for (int i = 0; i < MAX_MSG_TRACK; i++) {
            if (g_msg_track[i].active && g_msg_track[i].packet_id == hdr.packet_id) {
                uint64_t latency_us = now_us - g_msg_track[i].sent_us;
                metrics_record_packet_delivered(&g_metrics, latency_us);
                g_msg_track[i].active = false;
                break;
            }
        }
        /* Emit delivery event for visualization */
        fprintf(stdout,
            "{\"type\":\"message_delivered\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"packet_id\":\"0x%08X\"}\n",
            (unsigned long long)now_us, rx->id, hdr.packet_id);
        fflush(stdout);
        return;
    }

    /* Not final dest — forward */
    uint8_t hop_limit = hdr.hop_limit;
    forward_result_t fwd_res = forward_data(&rx->routes, hdr.dest_addr, &hop_limit, now_ms);

    if (fwd_res.route_error) {
        /* Build and broadcast RERR */
        bramble_rerr_t rerr = rerr_build(rx->addr, hdr.dest_addr, fwd_res.next_hop);
        outbound_packet_t pkt = {0};
        bramble_rerr_serialize(&rerr, pkt.data, RERR_SIZE);
        pkt.len = RERR_SIZE;
        pkt.is_broadcast = true;
        pkt.dest_addr = 0xFFFFFFFF;
        pkt.pkt_type = PKT_TYPE_RERR;
        sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                            &g_events, &g_metrics, now_us);
        emit_packet_dropped(stdout, now_us, rx->id, "no_route");
        return;
    }

    if (!fwd_res.should_send) return;

    /* Patch hop_limit in buffer and forward */
    uint8_t fwd_buf[256];
    memcpy(fwd_buf, buf, len);
    fwd_buf[3] = hop_limit;  /* hop_limit is byte 3 in the header */

    outbound_packet_t pkt = {0};
    memcpy(pkt.data, fwd_buf, len);
    pkt.len = len;
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;
    rx->packets_forwarded++;

    sim_radio_broadcast(rx, &pkt, &g_nodes, &g_radio, &g_rng,
                        &g_events, &g_metrics, now_us);
}
```

---

### Task 3.6 — Implement `EVT_GENERATE_MESSAGE` handler

**File:** `bramble/simulator/engine/main.c` — replace the TODO stub:

```c
case EVT_GENERATE_MESSAGE: {
    sim_node_t *src = node_array_find_by_id(&g_nodes, event->data.node.node_id);
    if (!src || !src->active) break;

    uint32_t dest_addr = event->data.node.addr;
    uint32_t now_ms = (uint32_t)(event->timestamp_us / 1000);

    /* Look up route */
    route_entry_t *route = route_lookup(&src->routes, dest_addr);

    if (!route || route->state == ROUTE_BROKEN || route->state == ROUTE_DISCOVERING) {
        /* Start or check pending discovery */
        pending_discovery_t *pd = discovery_lookup(&src->pending_discoveries, dest_addr);
        if (!pd) {
            uint32_t query_id = pcg32_random(&g_rng);
            discovery_start(&src->pending_discoveries, dest_addr, query_id, now_ms);

            bramble_rreq_t rreq = rreq_build_originator(src->addr, dest_addr, query_id, src->addr);

            outbound_packet_t pkt = {0};
            bramble_rreq_serialize(&rreq, pkt.data, RREQ_SIZE);
            pkt.len = RREQ_SIZE;
            pkt.is_broadcast = true;
            pkt.dest_addr = 0xFFFFFFFF;
            pkt.pkt_type = PKT_TYPE_RREQ;

            sim_radio_broadcast(src, &pkt, &g_nodes, &g_radio, &g_rng,
                                &g_events, &g_metrics, event->timestamp_us);
        }
        /* Data packet will be sent after RREP arrives (not queued in this impl — reschedule) */
        /* Reschedule message generation 6s later (after discovery completes) */
        sim_event_t retry = *event;
        retry.timestamp_us += 6000000ULL;
        event_queue_push(&g_events, &retry);
        break;
    }

    /* Route exists — build and send DATA packet */
    uint8_t hop_limit = 8;
    forward_result_t fwd_res = forward_data(&src->routes, dest_addr, &hop_limit, now_ms);
    if (!fwd_res.should_send) break;

    /* Build minimal data packet using header only (payload omitted in sim) */
    bramble_header_t hdr = {
        .version   = BRAMBLE_VERSION,
        .type      = PKT_TYPE_DATA,
        .flags     = 0,
        .hop_limit = hop_limit,
        .dest_addr = dest_addr,
        .packet_id = pcg32_random(&g_rng),
    };

    uint8_t buf[HEADER_SIZE];
    bramble_header_serialize(&hdr, buf, HEADER_SIZE);

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

    outbound_packet_t pkt = {0};
    memcpy(pkt.data, buf, HEADER_SIZE);
    pkt.len = HEADER_SIZE;
    pkt.is_broadcast = false;
    pkt.dest_addr = fwd_res.next_hop;
    pkt.pkt_type = PKT_TYPE_DATA;
    src->packets_originated++;

    sim_radio_broadcast(src, &pkt, &g_nodes, &g_radio, &g_rng,
                        &g_events, &g_metrics, event->timestamp_us);

    fprintf(stdout,
        "{\"type\":\"message_sent\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"dest\":\"0x%08X\",\"packet_id\":\"0x%08X\"}\n",
        (unsigned long long)event->timestamp_us, src->id, dest_addr, hdr.packet_id);
    fflush(stdout);
    break;
}
```

**Verify:**
```bash
make -C bramble/simulator/engine
./bramble-sim scenarios/3-node-linear.json 2>/dev/null | grep '"type"' | sort | uniq -c | sort -rn
# Expected: packet_sent, packet_received, route_added, node_joined all appear
# message_delivered should appear for send_message events in 3-node-linear.json
```

**Commit:** `git -C bramble commit -am "sim: full packet routing pipeline (RREQ/RREP/RERR/DATA)"`

---

## Section 4: Playback Controls

### Task 4.1 — Add server-side event buffer and playback state to relay.ts

The C engine runs as fast as possible, emitting all events to stdout. The server buffers them and delivers to the browser based on `timestamp_us × speedMultiplier`.

**File:** `bramble/simulator/server/relay.ts` — replace the `startSimulator` function with a buffering version:

```typescript
interface SimEvent {
  type: string;
  timestamp_us: number;
  [key: string]: unknown;
}

type PlaybackState = 'stopped' | 'playing' | 'paused';

function startSimulator(ws: WebSocket, scenarioPath: string) {
  const eventBuffer: SimEvent[] = [];
  let playbackState: PlaybackState = 'stopped';
  let speedMultiplier = 1.0;
  let playbackTimer: ReturnType<typeof setTimeout> | null = null;
  let nextEventIdx = 0;
  let playbackStartWallMs = 0;
  let playbackStartSimUs = 0;

  // Collect ALL events from C engine first, then start playback
  const sim = spawn(ENGINE_BIN, [scenarioPath], { stdio: ['ignore', 'pipe', 'pipe'] });
  let stdoutBuf = '';

  sim.stdout?.on('data', (chunk: Buffer) => {
    stdoutBuf += chunk.toString();
    const lines = stdoutBuf.split('\n');
    stdoutBuf = lines.pop() ?? '';
    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed.startsWith('{')) continue;
      try {
        const evt = JSON.parse(trimmed) as SimEvent;
        eventBuffer.push(evt);
      } catch { /* ignore malformed */ }
    }
  });

  sim.stderr?.on('data', (chunk: Buffer) => {
    process.stderr.write(`[sim] ${chunk.toString()}`);
  });

  sim.on('exit', () => {
    console.log(`[relay] Engine done, buffered ${eventBuffer.length} events`);
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'ready', eventCount: eventBuffer.length }));
      // Auto-play at 1x
      startPlayback();
    }
  });

  function startPlayback() {
    if (eventBuffer.length === 0) return;
    playbackState = 'playing';
    nextEventIdx = 0;
    playbackStartWallMs = Date.now();
    playbackStartSimUs = eventBuffer[0]?.timestamp_us ?? 0;
    scheduleNext();
  }

  function scheduleNext() {
    if (playbackState !== 'playing') return;
    if (nextEventIdx >= eventBuffer.length) {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'sim_ended', code: 0, signal: null }));
      }
      return;
    }

    const evt = eventBuffer[nextEventIdx];
    const simOffsetUs = evt.timestamp_us - playbackStartSimUs;
    const wallOffsetMs = (simOffsetUs / 1000) / speedMultiplier;
    const elapsedWallMs = Date.now() - playbackStartWallMs;
    const delayMs = Math.max(0, wallOffsetMs - elapsedWallMs);

    playbackTimer = setTimeout(() => {
      if (playbackState !== 'playing') return;
      // Send all events at or before current sim time
      const nowSimUs = playbackStartSimUs + (Date.now() - playbackStartWallMs) * 1000 * speedMultiplier;
      while (nextEventIdx < eventBuffer.length && eventBuffer[nextEventIdx].timestamp_us <= nowSimUs) {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify(eventBuffer[nextEventIdx]));
        }
        nextEventIdx++;
      }
      scheduleNext();
    }, delayMs);
  }

  function handleControl(msg: { type: string; speed?: number }) {
    switch (msg.type) {
      case 'play':
        if (playbackState === 'paused') {
          playbackStartWallMs = Date.now();
          playbackStartSimUs = eventBuffer[nextEventIdx]?.timestamp_us ?? 0;
          playbackState = 'playing';
          scheduleNext();
        } else if (playbackState === 'stopped') {
          startPlayback();
        }
        break;
      case 'pause':
        playbackState = 'paused';
        if (playbackTimer) { clearTimeout(playbackTimer); playbackTimer = null; }
        break;
      case 'speed':
        if (typeof msg.speed === 'number' && msg.speed > 0) {
          // Recalibrate: save current sim time, adjust wall baseline
          const curSimUs = eventBuffer[nextEventIdx]?.timestamp_us ?? 0;
          speedMultiplier = msg.speed;
          playbackStartWallMs = Date.now();
          playbackStartSimUs = curSimUs;
          if (playbackTimer) { clearTimeout(playbackTimer); playbackTimer = null; }
          if (playbackState === 'playing') scheduleNext();
        }
        break;
      case 'step':
        if (nextEventIdx < eventBuffer.length) {
          if (ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify(eventBuffer[nextEventIdx]));
          }
          nextEventIdx++;
        }
        break;
    }
  }

  return { handleControl, cleanup: () => { sim.kill('SIGTERM'); } };
}
```

**File:** `bramble/simulator/server/relay.ts` — update the WebSocket `message` handler to route control messages:

```typescript
// In wss.on('connection', ...) replace the ws.on('message', ...) block:
let simControl: { handleControl: (m: any) => void; cleanup: () => void } | null = null;

ws.on('message', (data: Buffer) => {
  try {
    const msg = JSON.parse(data.toString());
    if (msg.type === 'start') {
      const scenarioName = msg.scenario as string | undefined;
      const scenarioPath = scenarioName
        ? path.resolve(SCENARIOS_DIR, `${scenarioName}.json`)
        : DEFAULT_SCENARIO;
      if (!fs.existsSync(scenarioPath)) {
        ws.send(JSON.stringify({ type: 'error', message: `Scenario not found: ${scenarioPath}` }));
        return;
      }
      simControl = startSimulator(ws, scenarioPath);
    } else if (simControl && ['play','pause','speed','step'].includes(msg.type)) {
      simControl.handleControl(msg);
    }
  } catch { /* ignore */ }
});

ws.on('close', () => {
  simControl?.cleanup();
  simControl = null;
});
```

**Verify:**
```bash
cd bramble/simulator/server && npx tsc --noEmit
# Expected: no errors
```

---

### Task 4.2 — Wire playback controls in the UI

**File:** `bramble/simulator/ui/src/hooks/useSimulation.ts`

Add `sendControl()` to the returned object and handle the `ready` event:

```typescript
// Add to parseEvent():
case 'ready': {
  actions.push({ type: 'SIM_READY', eventCount: raw.eventCount as number });
  break;
}

// Add to simReducer():
case 'SIM_READY':
  return { ...state, ready: true, totalEvents: action.eventCount };

// Add to initialState:
ready: false,
totalEvents: 0,

// Change return value of useSimulation():
const sendControl = (msg: object) => {
  if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
    wsRef.current.send(JSON.stringify(msg));
  }
};
return { state, sendControl };
```

**File:** `bramble/simulator/ui/src/components/PlaybackControls.tsx`

Replace the static display with interactive controls:

```tsx
interface PlaybackControlsProps {
  running: boolean;
  connected: boolean;
  ready: boolean;
  currentTime: number;
  onPlay: () => void;
  onPause: () => void;
  onSpeed: (x: number) => void;
  onStep: () => void;
}

export function PlaybackControls({
  running, connected, ready, currentTime,
  onPlay, onPause, onSpeed, onStep,
}: PlaybackControlsProps) {
  const [speed, setSpeed] = React.useState(1);
  const speeds = [0.5, 1, 2, 5, 10, 25, 100];

  return (
    <header style={{ /* existing styles */ }}>
      {/* Title — unchanged */}

      {/* Playback buttons */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
        <button
          onClick={running ? onPause : onPlay}
          disabled={!ready}
          style={{ /* button style */ }}
        >
          {running ? '⏸' : '▶'}
        </button>
        <button onClick={onStep} disabled={!ready || running}
          style={{ /* button style */ }}>
          ⏭
        </button>
        <select
          value={speed}
          onChange={e => { const v = Number(e.target.value); setSpeed(v); onSpeed(v); }}
          style={{ /* select style */ }}
        >
          {speeds.map(s => <option key={s} value={s}>{s}x</option>)}
        </select>
      </div>

      {/* Sim time display — unchanged */}

      {/* Status indicator — unchanged */}
    </header>
  );
}
```

**File:** `bramble/simulator/ui/src/App.tsx` — pass handlers:

```tsx
const { state, sendControl } = useSimulation();

<PlaybackControls
  running={state.running}
  connected={state.connected}
  ready={state.ready}
  currentTime={state.currentTime}
  onPlay={() => sendControl({ type: 'play' })}
  onPause={() => sendControl({ type: 'pause' })}
  onSpeed={(x) => sendControl({ type: 'speed', speed: x })}
  onStep={() => sendControl({ type: 'step' })}
/>
```

**Verify:**
```bash
cd bramble/simulator/ui && npm run build
# Expected: build succeeds
```

**Commit:** `git -C bramble commit -am "sim: playback controls (play/pause/speed/step)"`

---

## Section 5: Animated Packets on Canvas

### Task 5.1 — Add `PacketAnimation` type and state to useSimulation

**File:** `bramble/simulator/ui/src/types.ts` — add:

```typescript
export interface PacketAnimation {
  id: number;
  fromNodeId: string;
  toNodeId: string;    // may be empty string for broadcasts
  pktType: 'RREQ' | 'RREP' | 'RERR' | 'BEACON' | 'DATA';
  startedAt: number;   // performance.now() when animation began
  durationMs: number;  // how long to travel (proportional to edge length)
}
```

**File:** `bramble/simulator/ui/src/hooks/useSimulation.ts`

Add to state and reducer:

```typescript
// initialState:
packets: [] as PacketAnimation[],
packetCounter: 0,

// In parseEvent() for 'packet_sent':
case 'packet_sent': {
  const pktType = (raw.pkt_type as string ?? 'DATA') as PacketAnimation['pktType'];
  // node = sender, dest = next hop addr; we need node id for dest
  // Store as srcNodeId + destAddr, canvas resolves to node id
  actions.push({
    type: 'ADD_PACKET',
    packet: {
      fromNodeId: raw.node as string,
      destAddr: raw.dest as string,
      pktType,
      timestamp_us,
    }
  });
  break;
}

// In reducer 'ADD_PACKET':
case 'ADD_PACKET': {
  const id = state.packetCounter + 1;
  const anim: PacketAnimation = {
    id,
    fromNodeId: action.packet.fromNodeId,
    toNodeId: action.packet.destAddr,  // resolved in canvas by addr→id
    pktType: action.packet.pktType,
    startedAt: performance.now(),
    durationMs: 400,
  };
  return {
    ...state,
    packets: [...state.packets.slice(-50), anim],  // keep last 50
    packetCounter: id,
  };
}

// In reducer 'TICK' (new action dispatched by requestAnimationFrame):
case 'TICK': {
  const now = performance.now();
  const packets = state.packets.filter(p => now - p.startedAt < p.durationMs);
  return { ...state, packets };
}
```

---

### Task 5.2 — Render animated packet dots in MeshCanvas

**File:** `bramble/simulator/ui/src/components/MeshCanvas.tsx`

Add `packets` to props and render animated `<circle>` elements:

```tsx
interface MeshCanvasProps {
  nodes: Map<string, SimNode>;
  packets: PacketAnimation[];
  radioRange?: number;
}

// Packet type colors:
const PKT_COLORS: Record<string, string> = {
  RREQ:   '#f0e040',  // yellow
  RREP:   '#58a6ff',  // blue
  RERR:   '#f85149',  // red
  BEACON: '#9e6adf',  // purple
  DATA:   '#3fb950',  // green
};

// Inside the SVG, add after links, before nodes:
{packets.map(pkt => {
  const fromNode = nodes.get(pkt.fromNodeId);
  // toNodeId may be an address string like "0xXXXXXXXX" — resolve to node
  const toNode = pkt.toNodeId === '0xFFFFFFFF'
    ? undefined  // broadcast — skip dot (or pick random neighbor)
    : Array.from(nodes.values()).find(n => `0x${n.addr?.toString(16).toUpperCase().padStart(8,'0')}` === pkt.toNodeId.toUpperCase());

  if (!fromNode || !toNode) return null;

  const now = performance.now();
  const t = Math.min(1, (now - pkt.startedAt) / pkt.durationMs);
  const { sx: x1, sy: y1 } = toScreen(fromNode.x, fromNode.y, transform);
  const { sx: x2, sy: y2 } = toScreen(toNode.x, toNode.y, transform);
  const cx = x1 + (x2 - x1) * t;
  const cy = y1 + (y2 - y1) * t;
  const color = PKT_COLORS[pkt.pktType] ?? '#ffffff';

  return (
    <g key={pkt.id}>
      <circle cx={cx} cy={cy} r={5} fill={color} opacity={0.9} />
      <circle cx={cx} cy={cy} r={8} fill={color} opacity={0.3} />
    </g>
  );
})}
```

**File:** `bramble/simulator/ui/src/App.tsx`

Drive animation with `requestAnimationFrame` and pass `packets` to canvas:

```tsx
// In App component:
const animFrameRef = useRef<number>(0);
useEffect(() => {
  function tick() {
    dispatch({ type: 'TICK' });
    animFrameRef.current = requestAnimationFrame(tick);
  }
  animFrameRef.current = requestAnimationFrame(tick);
  return () => cancelAnimationFrame(animFrameRef.current);
}, []);

// Pass to canvas:
<MeshCanvas nodes={state.nodes} packets={state.packets} radioRange={radioRange} />
```

> **Note:** `SimNode` needs an optional `addr?: number` field added to `types.ts` so the canvas can resolve address → node. Populate it from the `node_joined` event (add `addr` to the emitter's `emit_node_joined` JSON output).

**Verify:**
```bash
cd bramble/simulator/ui && npm run build
# Expected: build succeeds
```

**Commit:** `git -C bramble commit -am "sim: animated packet dots on canvas"`

---

## Section 6: Full Anomaly Detection Suite

### Task 6.1 — Add per-node anomaly tracker array to main.c

**File:** `bramble/simulator/engine/main.c` — add alongside existing globals:

```c
/* Per-node anomaly trackers (indexed by node array position) */
static route_flap_tracker_t g_flap_tracker[MAX_NODES];
static uint64_t g_last_fwd_us[MAX_NODES];         /* for black hole detection */
static uint32_t g_fwd_count[MAX_NODES];
static uint32_t g_fwd_no_deliver[MAX_NODES];

/* Initialize in main() after node_array_init: */
for (int i = 0; i < MAX_NODES; i++) {
    anomaly_init(&g_flap_tracker[i]);
    g_last_fwd_us[i] = 0;
    g_fwd_count[i] = 0;
    g_fwd_no_deliver[i] = 0;
}
```

---

### Task 6.2 — Detect route loops (packet visits same node twice)

**File:** `bramble/simulator/engine/sim_event.h` — add a `hop_path` field to `packet_event_data_t`:

```c
typedef struct {
    uint32_t src_addr;
    uint32_t dest_addr;
    int8_t   rssi;
    uint32_t hop_path[16];   /* ← addresses of nodes this packet has visited */
    uint8_t  hop_path_len;
    uint8_t  data[256];
    uint16_t len;
} packet_event_data_t;
```

**File:** `bramble/simulator/engine/main.c** — in `handle_data_received()`, before forwarding:

```c
/* Route loop detection */
for (int i = 0; i < event->data.packet.hop_path_len; i++) {
    if (event->data.packet.hop_path[i] == rx->addr) {
        emit_anomaly(stdout, now_us, "route_loop", rx->id, hdr.dest_addr,
                     "packet visited this node twice");
        emit_packet_dropped(stdout, now_us, rx->id, "route_loop");
        return;
    }
}
/* Add self to path */
if (event->data.packet.hop_path_len < 16) {
    event->data.packet.hop_path[event->data.packet.hop_path_len++] = rx->addr;
}
```

---

### Task 6.3 — Detect black holes and mesh partitions

**File:** `bramble/simulator/engine/sim_anomaly.h** — add new detection structs and functions:

```c
/* Black hole: a node that forwards many packets but none are delivered */
#define BLACKHOLE_FWD_THRESHOLD  20
#define BLACKHOLE_DELIVER_RATIO  0.10f   /* <10% of forwarded packets delivered */
#define BLACKHOLE_WINDOW_US      10000000ULL  /* 10s */

bool anomaly_check_blackhole(uint32_t fwd_count, uint32_t deliver_count,
                             uint64_t window_us, FILE *out,
                             const char *node_id);

/* Mesh partition: unreachable node after N seconds of inactivity */
#define PARTITION_TIMEOUT_US     30000000ULL  /* 30s */

void anomaly_check_partition(node_array_t *nodes, uint64_t now_us, FILE *out);

/* Excessive retransmissions: discovery attempted > MAX_RREQ_ATTEMPTS */
void anomaly_check_retransmissions(pending_discovery_table_t *table,
                                   uint64_t now_us, FILE *out, const char *node_id);
```

**File:** `bramble/simulator/engine/sim_anomaly.c** — implement:

```c
bool anomaly_check_blackhole(uint32_t fwd_count, uint32_t deliver_count,
                             uint64_t window_us, FILE *out, const char *node_id)
{
    (void)window_us;
    if (fwd_count < BLACKHOLE_FWD_THRESHOLD) return false;
    float ratio = fwd_count > 0 ? (float)deliver_count / (float)fwd_count : 0.0f;
    if (ratio < BLACKHOLE_DELIVER_RATIO) {
        fprintf(out,
            "{\"type\":\"anomaly\",\"timestamp_us\":0,\"kind\":\"black_hole\""
            ",\"node\":\"%s\",\"fwd\":%u,\"delivered\":%u}\n",
            node_id, fwd_count, deliver_count);
        fflush(out);
        return true;
    }
    return false;
}

void anomaly_check_partition(node_array_t *nodes, uint64_t now_us, FILE *out)
{
    for (int i = 0; i < nodes->count; i++) {
        sim_node_t *n = &nodes->nodes[i];
        if (!n->active) continue;
        if (n->packets_received == 0 && now_us > PARTITION_TIMEOUT_US) {
            fprintf(out,
                "{\"type\":\"anomaly\",\"timestamp_us\":%llu"
                ",\"kind\":\"partition\",\"node\":\"%s\",\"desc\":\"no packets received\"}\n",
                (unsigned long long)now_us, n->id);
            fflush(out);
        }
    }
}

void anomaly_check_retransmissions(pending_discovery_table_t *table,
                                   uint64_t now_us, FILE *out, const char *node_id)
{
    for (int i = 0; i < table->count; i++) {
        pending_discovery_t *d = &table->entries[i];
        if (d->attempts >= MAX_RREQ_ATTEMPTS) {
            fprintf(out,
                "{\"type\":\"anomaly\",\"timestamp_us\":%llu"
                ",\"kind\":\"excess_retransmit\",\"node\":\"%s\""
                ",\"dest\":\"0x%08X\",\"attempts\":%u}\n",
                (unsigned long long)now_us, node_id, d->dest_addr, d->attempts);
            fflush(out);
        }
    }
}
```

**Wire into** `EVT_METRICS_TICK` in `main.c`:

```c
case EVT_METRICS_TICK: {
    /* ... existing active node count code ... */

    /* Anomaly checks */
    anomaly_check_partition(&g_nodes, event->timestamp_us, stdout);
    for (int i = 0; i < g_nodes.count; i++) {
        sim_node_t *n = &g_nodes.nodes[i];
        if (!n->active) continue;
        anomaly_check_retransmissions(&n->pending_discoveries,
                                      event->timestamp_us, stdout, n->id);
        anomaly_check_blackhole(g_fwd_count[i], n->packets_received /* delivered approximation */,
                                NODE_BEACON_INTERVAL_US, stdout, n->id);
    }
    /* ... existing emit_metrics ... */
    break;
}
```

**Verify:**
```bash
make -C bramble/simulator/engine
# Expected: clean compile
```

**Commit:** `git -C bramble commit -am "sim: anomaly detection suite (blackhole/partition/loop/retransmit)"`

---

## Section 7: Stochastic Scenario Mode

### Task 7.1 — Implement stochastic scenario loader in sim_scenario.c

**File:** `bramble/simulator/engine/sim_scenario.c`

Add a `load_stochastic()` function (called when `mode == "stochastic"`):

```c
static bool load_stochastic(cJSON *root, scenario_t *scenario) {
    /* Read stochastic params */
    cJSON *nodes_cfg = cJSON_GetObjectItem(root, "nodes");
    cJSON *chaos_cfg = cJSON_GetObjectItem(root, "chaos");
    cJSON *traffic_cfg = cJSON_GetObjectItem(root, "traffic");

    int node_count = 10;
    float area_w = 500.0f, area_h = 500.0f;

    if (nodes_cfg) {
        cJSON *count = cJSON_GetObjectItem(nodes_cfg, "count");
        cJSON *area  = cJSON_GetObjectItem(nodes_cfg, "area");
        if (count && cJSON_IsNumber(count)) node_count = count->valueint;
        if (area  && cJSON_IsArray(area) && cJSON_GetArraySize(area) == 2) {
            area_w = (float)cJSON_GetArrayItem(area, 0)->valuedouble;
            area_h = (float)cJSON_GetArrayItem(area, 1)->valuedouble;
        }
    }

    /* Place nodes randomly */
    char node_id[NODE_ID_LEN];
    for (int i = 0; i < node_count && i < MAX_NODES; i++) {
        snprintf(node_id, sizeof(node_id), "N%02d", i);
        float x = pcg32_float(scenario->rng) * area_w;
        float y = pcg32_float(scenario->rng) * area_h;
        uint32_t addr = 0x10000000 + (uint32_t)i;
        node_array_add(scenario->nodes, node_id, addr, x, y);
    }

    /* Generate churn events */
    float join_rate = 2.0f, leave_rate = 1.0f;
    if (chaos_cfg) {
        cJSON *churn = cJSON_GetObjectItem(chaos_cfg, "node_churn");
        if (churn) {
            cJSON *jr = cJSON_GetObjectItem(churn, "join_rate_per_min");
            cJSON *lr = cJSON_GetObjectItem(churn, "leave_rate_per_min");
            if (jr && cJSON_IsNumber(jr)) join_rate  = (float)jr->valuedouble;
            if (lr && cJSON_IsNumber(lr)) leave_rate = (float)lr->valuedouble;
        }
    }

    uint64_t duration_us = scenario->metadata.duration_us;
    uint64_t t = 5000000ULL;  /* start churn after 5s */
    float join_interval_us  = 60000000.0f / join_rate;
    float leave_interval_us = 60000000.0f / leave_rate;

    while (t < duration_us) {
        /* Random join */
        if (pcg32_float(scenario->rng) < 0.5f) {
            t += (uint64_t)(pcg32_float(scenario->rng) * join_interval_us * 2);
            sim_event_t evt = {0};
            evt.type = EVT_NODE_JOIN;
            evt.timestamp_us = t;
            /* Pick a random inactive node or reuse last one */
            int idx = pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count - 1);
            strncpy(evt.data.node.node_id, scenario->nodes->nodes[idx].id, NODE_ID_LEN - 1);
            evt.data.node.x = pcg32_float(scenario->rng) * area_w;
            evt.data.node.y = pcg32_float(scenario->rng) * area_h;
            event_queue_push(scenario->events, &evt);
        } else {
            t += (uint64_t)(pcg32_float(scenario->rng) * leave_interval_us * 2);
            sim_event_t evt = {0};
            evt.type = EVT_NODE_LEAVE;
            evt.timestamp_us = t;
            int idx = pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count - 1);
            strncpy(evt.data.node.node_id, scenario->nodes->nodes[idx].id, NODE_ID_LEN - 1);
            event_queue_push(scenario->events, &evt);
        }
    }

    /* Generate traffic events */
    float msgs_per_min = 5.0f;
    if (traffic_cfg) {
        cJSON *mpm = cJSON_GetObjectItem(traffic_cfg, "messages_per_min");
        if (mpm && cJSON_IsNumber(mpm)) msgs_per_min = (float)mpm->valuedouble;
    }

    float msg_interval_us = 60000000.0f / msgs_per_min;
    t = 2000000ULL;  /* start traffic after 2s */
    while (t < duration_us) {
        t += (uint64_t)(pcg32_float(scenario->rng) * msg_interval_us * 2);
        int src_idx  = pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count - 1);
        int dest_idx = pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count - 1);
        if (src_idx == dest_idx) continue;

        sim_event_t evt = {0};
        evt.type = EVT_GENERATE_MESSAGE;
        evt.timestamp_us = t;
        strncpy(evt.data.node.node_id, scenario->nodes->nodes[src_idx].id, NODE_ID_LEN - 1);
        evt.data.node.addr = scenario->nodes->nodes[dest_idx].addr;
        event_queue_push(scenario->events, &evt);
    }

    /* Generate interference events */
    if (chaos_cfg) {
        cJSON *interf = cJSON_GetObjectItem(chaos_cfg, "interference");
        if (interf) {
            cJSON *freq = cJSON_GetObjectItem(interf, "frequency_per_min");
            float f = freq && cJSON_IsNumber(freq) ? (float)freq->valuedouble : 3.0f;
            float interf_interval_us = 60000000.0f / f;
            t = 3000000ULL;
            while (t < duration_us) {
                t += (uint64_t)(pcg32_float(scenario->rng) * interf_interval_us * 2);
                sim_event_t start_evt = {0};
                start_evt.type = EVT_INTERFERENCE_START;
                start_evt.timestamp_us = t;
                start_evt.data.interference.center_x = pcg32_float(scenario->rng) * area_w;
                start_evt.data.interference.center_y = pcg32_float(scenario->rng) * area_h;
                start_evt.data.interference.radius = 30.0f + pcg32_float(scenario->rng) * 70.0f;
                start_evt.data.interference.zone_index = -1;
                event_queue_push(scenario->events, &start_evt);

                sim_event_t end_evt = start_evt;
                end_evt.type = EVT_INTERFERENCE_END;
                end_evt.timestamp_us = t + 1000000ULL + (uint64_t)(pcg32_float(scenario->rng) * 4000000.0f);
                event_queue_push(scenario->events, &end_evt);
            }
        }
    }

    return true;
}
```

**Wire into `scenario_load_file()`:** After loading nodes from deterministic path, check mode:

```c
cJSON *mode_json = cJSON_GetObjectItem(root, "mode");
if (mode_json && strcmp(mode_json->valuestring, "stochastic") == 0) {
    scenario->metadata.deterministic = false;
    if (!load_stochastic(root, scenario)) { cJSON_Delete(root); return false; }
} else {
    if (!load_nodes(nodes_json, scenario->nodes)) { cJSON_Delete(root); return false; }
    if (!load_events(events_json, scenario->events, scenario->nodes, scenario->radio)) {
        cJSON_Delete(root); return false;
    }
}
```

**Verify:**
```bash
make -C bramble/simulator/engine
./bramble-sim scenarios/stochastic-stress.json 2>&1 | tail -5
# Expected: simulation runs, events emitted, no crash
```

**Commit:** `git -C bramble commit -am "sim: stochastic scenario generator"`

---

## Section 8: Scenario Upload UI

### Task 8.1 — Add upload endpoint to relay.ts

**File:** `bramble/simulator/server/relay.ts`

Add a `POST /api/scenarios/upload` handler in the HTTP server callback:

```typescript
// In server = http.createServer((req, res) => { ... }):

if (req.method === 'POST' && req.url === '/api/scenarios/upload') {
  let body = '';
  req.on('data', (chunk: Buffer) => { body += chunk.toString(); });
  req.on('end', () => {
    try {
      // Validate it's parseable JSON with a name field
      const scenario = JSON.parse(body) as { name?: string };
      const name = scenario.name?.replace(/[^a-zA-Z0-9_-]/g, '_') ?? 'uploaded';
      const dest = path.resolve(SCENARIOS_DIR, `${name}.json`);
      // Security: ensure dest is inside SCENARIOS_DIR
      if (!dest.startsWith(SCENARIOS_DIR)) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'invalid name' }));
        return;
      }
      fs.writeFileSync(dest, JSON.stringify(scenario, null, 2));
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true, scenario: name }));
    } catch (err) {
      res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: String(err) }));
    }
  });
  return;
}
```

---

### Task 8.2 — Add file picker to ScenarioLoader component

**File:** `bramble/simulator/ui/src/components/ScenarioLoader.tsx`

Add an upload button alongside the scenario selector:

```tsx
export function ScenarioLoader({ onScenarioSelect }: { onScenarioSelect: (name: string) => void }) {
  const [scenarios, setScenarios] = React.useState<string[]>([]);
  const [selected, setSelected] = React.useState('');
  const fileRef = React.useRef<HTMLInputElement>(null);

  React.useEffect(() => {
    fetch('/api/scenarios').then(r => r.json()).then(setScenarios);
  }, []);

  const handleUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    const text = await file.text();
    const res = await fetch('/api/scenarios/upload', {
      method: 'POST',
      body: text,
      headers: { 'Content-Type': 'application/json' },
    });
    const { ok, scenario, error } = await res.json();
    if (ok) {
      setScenarios(prev => [...prev, scenario]);
      setSelected(scenario);
    } else {
      alert(`Upload failed: ${error}`);
    }
  };

  return (
    <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
      <select value={selected} onChange={e => setSelected(e.target.value)}>
        <option value="">— select scenario —</option>
        {scenarios.map(s => <option key={s} value={s}>{s}</option>)}
      </select>
      <button onClick={() => selected && onScenarioSelect(selected)} disabled={!selected}>
        Run
      </button>
      <button onClick={() => fileRef.current?.click()}>
        Upload
      </button>
      <input ref={fileRef} type="file" accept=".json" style={{ display: 'none' }}
             onChange={handleUpload} />
    </div>
  );
}
```

**Verify:**
```bash
cd bramble/simulator/ui && npm run build
cd bramble/simulator/server && npx tsc --noEmit
```

**Commit:** `git -C bramble commit -am "sim: scenario upload UI + server endpoint"`

---

## Integration Test

### Task 9.1 — End-to-end smoke test

Verify the full pipeline works: C engine → server → browser.

```bash
# 1. Build everything
make -C bramble/simulator/engine

# 2. Run engine on 3-node scenario and check routing events
./bramble/simulator/engine/bramble-sim bramble/simulator/scenarios/3-node-linear.json \
  2>/dev/null \
  | jq -r '.type' \
  | sort | uniq -c | sort -rn
```

Expected output (counts will vary but all types should appear):
```
   N  packet_received
   N  packet_sent
   N  route_added
   3  node_joined
   1  config
   1  message_delivered   ← key: delivery happened
   1  message_sent
```

```bash
# 3. Build UI
cd bramble/simulator/ui && npm run build

# 4. Check TypeScript server compiles
cd ../server && npx tsc --noEmit
```

**Commit:** `git -C bramble commit -am "sim: phase 2 complete - integration verified"`

---

## Delivery Checklist

- [ ] `EVT_TICK_NODE` drives per-node beacon TX, neighbor purge, route maintenance, discovery retry
- [ ] `EVT_RECEIVE_PACKET` dispatches to correct protocol handler based on `bramble_header_t.type`
- [ ] `handle_rreq_received` uses `rreq_dedup_check_and_add`, `reverse_route_add`, `rreq_forward`, `rrep_build_destination`
- [ ] `handle_rrep_received` uses `route_install`, `discovery_lookup_by_query`, `rrep_forward`
- [ ] `handle_rerr_received` uses `rerr_handle`
- [ ] `handle_data_received` uses `forward_data`, `rerr_build`
- [ ] `EVT_GENERATE_MESSAGE` uses `discovery_start`, `rreq_build_originator`, `forward_data`
- [ ] Server buffers all events before streaming; responds to play/pause/speed/step
- [ ] PlaybackControls sends WS commands; state.ready gates the buttons
- [ ] Packet dots animate on SVG canvas with type-correct colors
- [ ] Anomaly detection: route_flap, black_hole, partition, route_loop, excess_retransmit
- [ ] Stochastic loader generates nodes/traffic/churn/interference from seed
- [ ] Upload endpoint validates and stores JSON scenarios

---

## Future Enhancements (Out of Scope)

### Level 2: QEMU ESP32 Emulation
Run actual firmware binaries in Espressif's QEMU ESP32 fork. Each simulated node is a full QEMU instance running the real firmware with FreeRTOS. Virtual radio bridge connects instances. Extremely realistic but requires complete firmware + custom radio HAL mocking.

### Level 3: ESP-IDF Linux Target  
Compile firmware as native Linux binaries using ESP-IDF's experimental POSIX host support. Lighter than QEMU but same concept — run real firmware, mock the radio layer.

Both approaches become valuable once the firmware is more complete and hardware integration testing begins. The Level 1 tick-based architecture designed here maps directly to what FreeRTOS tasks will look like, serving as design validation.
