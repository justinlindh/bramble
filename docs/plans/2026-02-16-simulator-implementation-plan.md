# Bramble Mesh Simulator Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Build a network simulator that runs actual Bramble C component code against a virtual mesh with real-time React visualization, all packaged in a single Docker container.

**Architecture:** Three-tier system — C simulation engine (discrete event queue + virtual nodes) → Node.js WebSocket relay → React UI with SVG mesh canvas. All Bramble component `.c` files included at compile time (zero modifications to existing code).

**Tech Stack:** C (gcc), cJSON (vendored), Node.js + TypeScript + ws, React 18 + Vite + TypeScript, SVG, Docker multi-stage build.

---

## Phase 1: Engine Foundation

### Task 1.1: Create simulator directory structure
```bash
cd /home/justin/.openclaw/workspace/bramble
mkdir -p simulator/engine
mkdir -p simulator/server
mkdir -p simulator/ui/src/components
mkdir -p simulator/ui/src/hooks
mkdir -p simulator/scenarios
```
**Verify:** `tree -L 2 simulator/` shows all directories

**Commit:** `git add simulator/ && git commit -m "simulator: create directory structure"`

---

### Task 1.2: Vendor cJSON library
Download single-file JSON library:
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/engine
curl -O https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
curl -O https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
```
**Verify:** `ls -lh cJSON.{c,h}` shows both files ~70KB total

**Commit:** `git add engine/cJSON.* && git commit -m "simulator: vendor cJSON library"`

---

### Task 1.3: Create sim_event.h (event queue header)
**File:** `simulator/engine/sim_event.h`
```c
#ifndef SIM_EVENT_H
#define SIM_EVENT_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_EVENT_QUEUE 10000

typedef enum {
    EVT_SEND_PACKET,
    EVT_RECEIVE_PACKET,
    EVT_TIMER_FIRE,
    EVT_NODE_JOIN,
    EVT_NODE_LEAVE,
    EVT_NODE_MOVE,
    EVT_INTERFERENCE_START,
    EVT_INTERFERENCE_END,
    EVT_GENERATE_MESSAGE,
    EVT_METRICS_TICK,
} event_type_t;

typedef struct {
    uint8_t data[256];
    uint16_t len;
    uint8_t from_idx;
    uint8_t to_idx;
    int8_t rssi;
    int8_t snr;
} packet_event_data_t;

typedef struct {
    uint8_t node_idx;
    float x;
    float y;
} node_event_data_t;

typedef struct {
    float center_x;
    float center_y;
    float radius;
} interference_event_data_t;

typedef union {
    packet_event_data_t packet;
    node_event_data_t node;
    interference_event_data_t interference;
    uint8_t node_idx;
    uint32_t timer_id;
} event_data_t;

typedef struct {
    uint64_t timestamp_us;
    event_type_t type;
    event_data_t data;
} sim_event_t;

typedef struct {
    sim_event_t events[MAX_EVENT_QUEUE];
    int count;
} event_queue_t;

void event_queue_init(event_queue_t *q);
bool event_queue_push(event_queue_t *q, const sim_event_t *evt);
bool event_queue_pop(event_queue_t *q, sim_event_t *evt);
bool event_queue_peek(const event_queue_t *q, sim_event_t *evt);
int  event_queue_count(const event_queue_t *q);

#endif /* SIM_EVENT_H */
```

**Commit:** `git add engine/sim_event.h && git commit -m "simulator: add event queue header"`

---

### Task 1.4: Implement sim_event.c (priority queue)
**File:** `simulator/engine/sim_event.c`
```c
#include "sim_event.h"
#include <string.h>

void event_queue_init(event_queue_t *q) {
    q->count = 0;
}

bool event_queue_push(event_queue_t *q, const sim_event_t *evt) {
    if (q->count >= MAX_EVENT_QUEUE) {
        return false;
    }

    /* Insert at sorted position (min-heap by timestamp) */
    int pos = q->count;
    q->events[pos] = *evt;
    q->count++;

    /* Bubble up */
    while (pos > 0) {
        int parent = (pos - 1) / 2;
        if (q->events[pos].timestamp_us >= q->events[parent].timestamp_us) {
            break;
        }
        /* Swap */
        sim_event_t tmp = q->events[pos];
        q->events[pos] = q->events[parent];
        q->events[parent] = tmp;
        pos = parent;
    }

    return true;
}

bool event_queue_pop(event_queue_t *q, sim_event_t *evt) {
    if (q->count == 0) {
        return false;
    }

    *evt = q->events[0];

    /* Move last element to root */
    q->count--;
    if (q->count > 0) {
        q->events[0] = q->events[q->count];

        /* Bubble down */
        int pos = 0;
        while (true) {
            int left = 2 * pos + 1;
            int right = 2 * pos + 2;
            int smallest = pos;

            if (left < q->count && q->events[left].timestamp_us < q->events[smallest].timestamp_us) {
                smallest = left;
            }
            if (right < q->count && q->events[right].timestamp_us < q->events[smallest].timestamp_us) {
                smallest = right;
            }

            if (smallest == pos) {
                break;
            }

            /* Swap */
            sim_event_t tmp = q->events[pos];
            q->events[pos] = q->events[smallest];
            q->events[smallest] = tmp;
            pos = smallest;
        }
    }

    return true;
}

bool event_queue_peek(const event_queue_t *q, sim_event_t *evt) {
    if (q->count == 0) {
        return false;
    }
    *evt = q->events[0];
    return true;
}

int event_queue_count(const event_queue_t *q) {
    return q->count;
}
```

**Commit:** `git add engine/sim_event.c && git commit -m "simulator: implement priority event queue"`

---

### Task 1.5: Create sim_random.h (seeded PRNG)
**File:** `simulator/engine/sim_random.h`
```c
#ifndef SIM_RANDOM_H
#define SIM_RANDOM_H

#include <stdint.h>

typedef struct {
    uint64_t state;
    uint64_t inc;
} pcg32_state_t;

void pcg32_seed(pcg32_state_t *rng, uint64_t seed);
uint32_t pcg32_random(pcg32_state_t *rng);
uint32_t pcg32_range(pcg32_state_t *rng, uint32_t min, uint32_t max);
float pcg32_float(pcg32_state_t *rng);

#endif /* SIM_RANDOM_H */
```

**Commit:** `git add engine/sim_random.h && git commit -m "simulator: add PRNG header"`

---

### Task 1.6: Implement sim_random.c (PCG32 algorithm)
**File:** `simulator/engine/sim_random.c`
```c
#include "sim_random.h"

void pcg32_seed(pcg32_state_t *rng, uint64_t seed) {
    rng->state = 0;
    rng->inc = (seed << 1) | 1;
    pcg32_random(rng);
    rng->state += seed;
    pcg32_random(rng);
}

uint32_t pcg32_random(pcg32_state_t *rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = ((oldstate >> 18) ^ oldstate) >> 27;
    uint32_t rot = oldstate >> 59;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

uint32_t pcg32_range(pcg32_state_t *rng, uint32_t min, uint32_t max) {
    uint32_t range = max - min + 1;
    return min + (pcg32_random(rng) % range);
}

float pcg32_float(pcg32_state_t *rng) {
    return (float)pcg32_random(rng) / (float)UINT32_MAX;
}
```

**Commit:** `git add engine/sim_random.c && git commit -m "simulator: implement PCG32 PRNG"`

---

### Task 1.7: Create sim_emitter.h (JSON event output)
**File:** `simulator/engine/sim_emitter.h`
```c
#ifndef SIM_EMITTER_H
#define SIM_EMITTER_H

#include <stdint.h>
#include <stdio.h>

void emit_packet_sent(FILE *out, uint64_t t_us, const char *from_id, const char *to_id, uint8_t pkt_type, uint32_t pkt_id);
void emit_packet_received(FILE *out, uint64_t t_us, const char *node_id, const char *from_id, uint8_t pkt_type, uint32_t pkt_id, int8_t rssi);
void emit_packet_dropped(FILE *out, uint64_t t_us, const char *node_id, const char *from_id, const char *reason);
void emit_route_added(FILE *out, uint64_t t_us, const char *node_id, const char *dest_id, const char *next_hop_id, uint8_t metric);
void emit_route_removed(FILE *out, uint64_t t_us, const char *node_id, const char *dest_id, const char *reason);
void emit_node_moved(FILE *out, uint64_t t_us, const char *node_id, float x, float y);
void emit_node_joined(FILE *out, uint64_t t_us, const char *node_id, float x, float y);
void emit_node_left(FILE *out, uint64_t t_us, const char *node_id);
void emit_link_broken(FILE *out, uint64_t t_us, const char *node_a, const char *node_b, const char *reason);
void emit_metrics(FILE *out, uint64_t t_us, float delivery_rate, float avg_latency_ms, int active_nodes, int total_packets, int dropped_packets);
void emit_anomaly(FILE *out, uint64_t t_us, const char *severity, const char *kind, const char *node_id, const char *desc);

#endif /* SIM_EMITTER_H */
```

**Commit:** `git add engine/sim_emitter.h && git commit -m "simulator: add JSON emitter header"`

---

### Task 1.8: Implement sim_emitter.c
**File:** `simulator/engine/sim_emitter.c`
```c
#include "sim_emitter.h"

void emit_packet_sent(FILE *out, uint64_t t_us, const char *from_id, const char *to_id, uint8_t pkt_type, uint32_t pkt_id) {
    fprintf(out, "{\"t\":%llu,\"type\":\"packet_sent\",\"from\":\"%s\",\"to\":\"%s\",\"packet_type\":%u,\"id\":%u}\n",
            t_us, from_id, to_id, pkt_type, pkt_id);
    fflush(out);
}

void emit_packet_received(FILE *out, uint64_t t_us, const char *node_id, const char *from_id, uint8_t pkt_type, uint32_t pkt_id, int8_t rssi) {
    fprintf(out, "{\"t\":%llu,\"type\":\"packet_received\",\"at\":\"%s\",\"from\":\"%s\",\"packet_type\":%u,\"id\":%u,\"rssi\":%d}\n",
            t_us, node_id, from_id, pkt_type, pkt_id, rssi);
    fflush(out);
}

void emit_packet_dropped(FILE *out, uint64_t t_us, const char *node_id, const char *from_id, const char *reason) {
    fprintf(out, "{\"t\":%llu,\"type\":\"packet_dropped\",\"at\":\"%s\",\"from\":\"%s\",\"reason\":\"%s\"}\n",
            t_us, node_id, from_id, reason);
    fflush(out);
}

void emit_route_added(FILE *out, uint64_t t_us, const char *node_id, const char *dest_id, const char *next_hop_id, uint8_t metric) {
    fprintf(out, "{\"t\":%llu,\"type\":\"route_added\",\"node\":\"%s\",\"dest\":\"%s\",\"next_hop\":\"%s\",\"metric\":%u}\n",
            t_us, node_id, dest_id, next_hop_id, metric);
    fflush(out);
}

void emit_route_removed(FILE *out, uint64_t t_us, const char *node_id, const char *dest_id, const char *reason) {
    fprintf(out, "{\"t\":%llu,\"type\":\"route_removed\",\"node\":\"%s\",\"dest\":\"%s\",\"reason\":\"%s\"}\n",
            t_us, node_id, dest_id, reason);
    fflush(out);
}

void emit_node_moved(FILE *out, uint64_t t_us, const char *node_id, float x, float y) {
    fprintf(out, "{\"t\":%llu,\"type\":\"node_moved\",\"node\":\"%s\",\"x\":%.2f,\"y\":%.2f}\n",
            t_us, node_id, x, y);
    fflush(out);
}

void emit_node_joined(FILE *out, uint64_t t_us, const char *node_id, float x, float y) {
    fprintf(out, "{\"t\":%llu,\"type\":\"node_joined\",\"node\":\"%s\",\"x\":%.2f,\"y\":%.2f}\n",
            t_us, node_id, x, y);
    fflush(out);
}

void emit_node_left(FILE *out, uint64_t t_us, const char *node_id) {
    fprintf(out, "{\"t\":%llu,\"type\":\"node_left\",\"node\":\"%s\"}\n",
            t_us, node_id);
    fflush(out);
}

void emit_link_broken(FILE *out, uint64_t t_us, const char *node_a, const char *node_b, const char *reason) {
    fprintf(out, "{\"t\":%llu,\"type\":\"link_broken\",\"between\":[\"%s\",\"%s\"],\"reason\":\"%s\"}\n",
            t_us, node_a, node_b, reason);
    fflush(out);
}

void emit_metrics(FILE *out, uint64_t t_us, float delivery_rate, float avg_latency_ms, int active_nodes, int total_packets, int dropped_packets) {
    fprintf(out, "{\"t\":%llu,\"type\":\"metrics\",\"delivery_rate\":%.3f,\"avg_latency_ms\":%.1f,\"active_nodes\":%d,\"total_packets\":%d,\"dropped_packets\":%d}\n",
            t_us, delivery_rate, avg_latency_ms, active_nodes, total_packets, dropped_packets);
    fflush(out);
}

void emit_anomaly(FILE *out, uint64_t t_us, const char *severity, const char *kind, const char *node_id, const char *desc) {
    fprintf(out, "{\"t\":%llu,\"type\":\"anomaly\",\"severity\":\"%s\",\"kind\":\"%s\",\"node\":\"%s\",\"desc\":\"%s\"}\n",
            t_us, severity, kind, node_id, desc);
    fflush(out);
}
```

**Commit:** `git add engine/sim_emitter.c && git commit -m "simulator: implement JSON event emitter"`

---

### Task 1.9: Create initial Makefile
**File:** `simulator/engine/Makefile`
```makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -I../../test/stubs -I../../components/routing/include -I../../components/packet/include

SIM_OBJS = sim_event.o sim_random.o sim_emitter.o cJSON.o

all: bramble-sim

bramble-sim: $(SIM_OBJS) main.o
	$(CC) $(CFLAGS) -o $@ $^

sim_event.o: sim_event.c sim_event.h
	$(CC) $(CFLAGS) -c $<

sim_random.o: sim_random.c sim_random.h
	$(CC) $(CFLAGS) -c $<

sim_emitter.o: sim_emitter.c sim_emitter.h
	$(CC) $(CFLAGS) -c $<

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) -c $<

main.o: main.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o bramble-sim

.PHONY: all clean
```

**Commit:** `git add engine/Makefile && git commit -m "simulator: add initial Makefile"`

---

## Phase 2: Virtual Node Management

### Task 2.1: Create sim_node.h
**File:** `simulator/engine/sim_node.h`
```c
#ifndef SIM_NODE_H
#define SIM_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include "routing.h"
#include "discovery.h"

#define MAX_NODES 64
#define NODE_ID_LEN 16

typedef struct {
    char id[NODE_ID_LEN];
    uint32_t addr;
    float x;
    float y;
    bool active;
    
    /* Bramble state (per-node instances) */
    routing_table_t routes;
    neighbor_table_t neighbors;
    reverse_route_table_t reverse_routes;
    rreq_dedup_t rreq_dedup;
    pending_discovery_table_t pending_discoveries;
    
    /* Stats */
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_forwarded;
    uint32_t packets_originated;
} sim_node_t;

typedef struct {
    sim_node_t nodes[MAX_NODES];
    int count;
} node_array_t;

void node_array_init(node_array_t *arr);
int node_array_add(node_array_t *arr, const char *id, uint32_t addr, float x, float y);
sim_node_t *node_array_find_by_id(node_array_t *arr, const char *id);
sim_node_t *node_array_find_by_addr(node_array_t *arr, uint32_t addr);
sim_node_t *node_array_get(node_array_t *arr, int idx);
void node_activate(sim_node_t *node);
void node_deactivate(sim_node_t *node);
void node_move(sim_node_t *node, float x, float y);

#endif /* SIM_NODE_H */
```

**Commit:** `git add engine/sim_node.h && git commit -m "simulator: add virtual node header"`

---

### Task 2.2: Implement sim_node.c
**File:** `simulator/engine/sim_node.c`
```c
#include "sim_node.h"
#include <string.h>

void node_array_init(node_array_t *arr) {
    arr->count = 0;
    memset(arr->nodes, 0, sizeof(arr->nodes));
}

int node_array_add(node_array_t *arr, const char *id, uint32_t addr, float x, float y) {
    if (arr->count >= MAX_NODES) {
        return -1;
    }
    
    sim_node_t *node = &arr->nodes[arr->count];
    strncpy(node->id, id, NODE_ID_LEN - 1);
    node->id[NODE_ID_LEN - 1] = '\0';
    node->addr = addr;
    node->x = x;
    node->y = y;
    node->active = true;
    
    /* Initialize Bramble tables */
    route_init(&node->routes);
    neighbor_init(&node->neighbors);
    reverse_route_init(&node->reverse_routes);
    rreq_dedup_init(&node->rreq_dedup);
    discovery_init(&node->pending_discoveries);
    
    /* Reset stats */
    node->packets_sent = 0;
    node->packets_received = 0;
    node->packets_forwarded = 0;
    node->packets_originated = 0;
    
    return arr->count++;
}

sim_node_t *node_array_find_by_id(node_array_t *arr, const char *id) {
    for (int i = 0; i < arr->count; i++) {
        if (strcmp(arr->nodes[i].id, id) == 0) {
            return &arr->nodes[i];
        }
    }
    return NULL;
}

sim_node_t *node_array_find_by_addr(node_array_t *arr, uint32_t addr) {
    for (int i = 0; i < arr->count; i++) {
        if (arr->nodes[i].addr == addr) {
            return &arr->nodes[i];
        }
    }
    return NULL;
}

sim_node_t *node_array_get(node_array_t *arr, int idx) {
    if (idx < 0 || idx >= arr->count) {
        return NULL;
    }
    return &arr->nodes[idx];
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
```

**Commit:** `git add engine/sim_node.c && git commit -m "simulator: implement virtual node management"`

---

### Task 2.3: Update Makefile for sim_node
**Edit:** `simulator/engine/Makefile`

Change:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o cJSON.o
```

To:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o cJSON.o
```

Add build rule:
```makefile
sim_node.o: sim_node.c sim_node.h
	$(CC) $(CFLAGS) -c $<
```

**Verify:** `cd simulator/engine && make clean` succeeds

**Commit:** `git add engine/Makefile && git commit -m "simulator: update Makefile for sim_node"`

---

## Phase 3: Radio Propagation Model

### Task 3.1: Create sim_radio.h
**File:** `simulator/engine/sim_radio.h`
```c
#ifndef SIM_RADIO_H
#define SIM_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "sim_node.h"
#include "sim_random.h"

#define MAX_INTERFERENCE_ZONES 16

typedef struct {
    float center_x;
    float center_y;
    float radius;
    bool active;
} interference_zone_t;

typedef struct {
    float range;
    float loss_pct;
    float propagation_speed_ms_per_unit;
    interference_zone_t zones[MAX_INTERFERENCE_ZONES];
    int zone_count;
} radio_config_t;

void radio_config_init(radio_config_t *cfg, float range, float loss_pct, float propagation_speed);
float radio_distance(const sim_node_t *a, const sim_node_t *b);
int8_t radio_compute_rssi(float distance, float range);
bool radio_can_receive(const sim_node_t *from, const sim_node_t *to, const radio_config_t *cfg, pcg32_state_t *rng);
uint64_t radio_propagation_delay_us(float distance, const radio_config_t *cfg);
int radio_add_interference_zone(radio_config_t *cfg, float x, float y, float radius);
void radio_clear_interference_zone(radio_config_t *cfg, int zone_idx);
bool radio_in_interference(float x, float y, const radio_config_t *cfg);

#endif /* SIM_RADIO_H */
```

**Commit:** `git add engine/sim_radio.h && git commit -m "simulator: add radio propagation header"`

---

### Task 3.2: Implement sim_radio.c
**File:** `simulator/engine/sim_radio.c`
```c
#include "sim_radio.h"
#include <string.h>

void radio_config_init(radio_config_t *cfg, float range, float loss_pct, float propagation_speed) {
    cfg->range = range;
    cfg->loss_pct = loss_pct;
    cfg->propagation_speed_ms_per_unit = propagation_speed;
    cfg->zone_count = 0;
    memset(cfg->zones, 0, sizeof(cfg->zones));
}

float radio_distance(const sim_node_t *a, const sim_node_t *b) {
    float dx = a->x - b->x;
    float dy = a->y - b->y;
    return sqrtf(dx * dx + dy * dy);
}

int8_t radio_compute_rssi(float distance, float range) {
    if (distance >= range) {
        return -128; /* Out of range */
    }
    
    /* Simple path loss model: RSSI = -40 - 20*log10(distance/10) */
    /* Clamp to reasonable range: -40 (very close) to -100 (edge) */
    float ratio = distance / range;
    int8_t rssi = -40 - (int8_t)(60.0f * ratio);
    
    if (rssi < -100) rssi = -100;
    if (rssi > -40) rssi = -40;
    
    return rssi;
}

bool radio_can_receive(const sim_node_t *from, const sim_node_t *to, const radio_config_t *cfg, pcg32_state_t *rng) {
    if (!from->active || !to->active) {
        return false;
    }
    
    float dist = radio_distance(from, to);
    if (dist >= cfg->range) {
        return false; /* Out of range */
    }
    
    /* Check interference zones */
    if (radio_in_interference(to->x, to->y, cfg)) {
        return false; /* Receiver in interference zone */
    }
    
    /* Apply random packet loss */
    if (cfg->loss_pct > 0.0f) {
        float roll = pcg32_float(rng) * 100.0f;
        if (roll < cfg->loss_pct) {
            return false; /* Packet lost */
        }
    }
    
    return true;
}

uint64_t radio_propagation_delay_us(float distance, const radio_config_t *cfg) {
    /* Delay in microseconds = distance * propagation_speed_ms * 1000 */
    return (uint64_t)(distance * cfg->propagation_speed_ms_per_unit * 1000.0f);
}

int radio_add_interference_zone(radio_config_t *cfg, float x, float y, float radius) {
    if (cfg->zone_count >= MAX_INTERFERENCE_ZONES) {
        return -1;
    }
    
    interference_zone_t *zone = &cfg->zones[cfg->zone_count];
    zone->center_x = x;
    zone->center_y = y;
    zone->radius = radius;
    zone->active = true;
    
    return cfg->zone_count++;
}

void radio_clear_interference_zone(radio_config_t *cfg, int zone_idx) {
    if (zone_idx >= 0 && zone_idx < cfg->zone_count) {
        cfg->zones[zone_idx].active = false;
    }
}

bool radio_in_interference(float x, float y, const radio_config_t *cfg) {
    for (int i = 0; i < cfg->zone_count; i++) {
        if (!cfg->zones[i].active) {
            continue;
        }
        
        float dx = x - cfg->zones[i].center_x;
        float dy = y - cfg->zones[i].center_y;
        float dist = sqrtf(dx * dx + dy * dy);
        
        if (dist <= cfg->zones[i].radius) {
            return true;
        }
    }
    return false;
}
```

**Commit:** `git add engine/sim_radio.c && git commit -m "simulator: implement radio propagation model"`

---

### Task 3.3: Update Makefile for sim_radio
**Edit:** `simulator/engine/Makefile`

Change:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o cJSON.o
```

To:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o cJSON.o
```

Add:
```makefile
sim_radio.o: sim_radio.c sim_radio.h
	$(CC) $(CFLAGS) -c $<
```

Add `-lm` to linker:
```makefile
bramble-sim: $(SIM_OBJS) main.o
	$(CC) $(CFLAGS) -o $@ $^ -lm
```

**Commit:** `git add engine/Makefile && git commit -m "simulator: update Makefile for sim_radio with math lib"`

---

## Phase 4: Scenario JSON Loader

### Task 4.1: Create sim_scenario.h
**File:** `simulator/engine/sim_scenario.h`
```c
#ifndef SIM_SCENARIO_H
#define SIM_SCENARIO_H

#include <stdint.h>
#include <stdbool.h>
#include "sim_node.h"
#include "sim_event.h"
#include "sim_radio.h"
#include "sim_random.h"

#define MAX_SCENARIO_EVENTS 1000

typedef struct {
    char name[64];
    bool deterministic;
    uint64_t seed;
    uint64_t duration_us;
} scenario_metadata_t;

typedef struct {
    scenario_metadata_t meta;
    node_array_t *nodes;
    radio_config_t *radio;
    event_queue_t *events;
    pcg32_state_t *rng;
} scenario_t;

bool scenario_load_file(const char *path, scenario_t *scenario);

#endif /* SIM_SCENARIO_H */
```

**Commit:** `git add engine/sim_scenario.h && git commit -m "simulator: add scenario loader header"`

---

### Task 4.2: Implement sim_scenario.c (deterministic only for now)
**File:** `simulator/engine/sim_scenario.c`
```c
#include "sim_scenario.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool load_deterministic(cJSON *root, scenario_t *scenario);

bool scenario_load_file(const char *path, scenario_t *scenario) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open scenario: %s\n", path);
        return false;
    }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json_str = malloc(fsize + 1);
    fread(json_str, 1, fsize, f);
    fclose(f);
    json_str[fsize] = '\0';
    
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) {
        fprintf(stderr, "JSON parse error\n");
        return false;
    }
    
    /* Parse metadata */
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (name && cJSON_IsString(name)) {
        strncpy(scenario->meta.name, name->valuestring, 63);
        scenario->meta.name[63] = '\0';
    }
    
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    scenario->meta.deterministic = (mode && strcmp(mode->valuestring, "deterministic") == 0);
    
    cJSON *duration = cJSON_GetObjectItem(root, "duration_ms");
    scenario->meta.duration_us = (duration ? duration->valueint : 30000) * 1000ULL;
    
    cJSON *seed = cJSON_GetObjectItem(root, "seed");
    scenario->meta.seed = seed ? seed->valueint : 42;
    
    /* Seed RNG */
    pcg32_seed(scenario->rng, scenario->meta.seed);
    
    bool success = false;
    if (scenario->meta.deterministic) {
        success = load_deterministic(root, scenario);
    } else {
        fprintf(stderr, "Stochastic mode not yet implemented\n");
    }
    
    cJSON_Delete(root);
    return success;
}

static bool load_deterministic(cJSON *root, scenario_t *scenario) {
    /* Load nodes */
    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    if (!cJSON_IsArray(nodes)) {
        fprintf(stderr, "Missing or invalid 'nodes' array\n");
        return false;
    }
    
    cJSON *node;
    uint32_t addr_counter = 0x01000000;
    cJSON_ArrayForEach(node, nodes) {
        cJSON *id = cJSON_GetObjectItem(node, "id");
        cJSON *x = cJSON_GetObjectItem(node, "x");
        cJSON *y = cJSON_GetObjectItem(node, "y");
        
        if (!id || !cJSON_IsString(id) || !x || !y) {
            continue;
        }
        
        node_array_add(scenario->nodes, id->valuestring, addr_counter++,
                      (float)x->valuedouble, (float)y->valuedouble);
    }
    
    /* Load radio config */
    cJSON *radio_obj = cJSON_GetObjectItem(root, "radio");
    if (cJSON_IsObject(radio_obj)) {
        cJSON *range = cJSON_GetObjectItem(radio_obj, "range");
        cJSON *loss = cJSON_GetObjectItem(radio_obj, "loss_pct");
        cJSON *speed = cJSON_GetObjectItem(radio_obj, "propagation_speed_ms_per_unit");
        
        radio_config_init(scenario->radio,
                         range ? (float)range->valuedouble : 150.0f,
                         loss ? (float)loss->valuedouble : 0.0f,
                         speed ? (float)speed->valuedouble : 0.1f);
    }
    
    /* Load scripted events */
    cJSON *events = cJSON_GetObjectItem(root, "events");
    if (cJSON_IsArray(events)) {
        cJSON *evt;
        cJSON_ArrayForEach(evt, events) {
            cJSON *at_ms = cJSON_GetObjectItem(evt, "at_ms");
            cJSON *type = cJSON_GetObjectItem(evt, "type");
            
            if (!at_ms || !type || !cJSON_IsString(type)) {
                continue;
            }
            
            uint64_t timestamp_us = at_ms->valueint * 1000ULL;
            sim_event_t sim_evt = {0};
            sim_evt.timestamp_us = timestamp_us;
            
            if (strcmp(type->valuestring, "send_message") == 0) {
                sim_evt.type = EVT_GENERATE_MESSAGE;
                /* Parse from/to/payload — will implement in main.c */
                event_queue_push(scenario->events, &sim_evt);
            } else if (strcmp(type->valuestring, "move_node") == 0) {
                cJSON *node_id = cJSON_GetObjectItem(evt, "node");
                cJSON *x = cJSON_GetObjectItem(evt, "x");
                cJSON *y = cJSON_GetObjectItem(evt, "y");
                
                if (node_id && x && y) {
                    sim_evt.type = EVT_NODE_MOVE;
                    sim_node_t *n = node_array_find_by_id(scenario->nodes, node_id->valuestring);
                    if (n) {
                        sim_evt.data.node.node_idx = n - scenario->nodes->nodes;
                        sim_evt.data.node.x = (float)x->valuedouble;
                        sim_evt.data.node.y = (float)y->valuedouble;
                        event_queue_push(scenario->events, &sim_evt);
                    }
                }
            } else if (strcmp(type->valuestring, "kill_node") == 0) {
                cJSON *node_id = cJSON_GetObjectItem(evt, "node");
                if (node_id) {
                    sim_evt.type = EVT_NODE_LEAVE;
                    sim_node_t *n = node_array_find_by_id(scenario->nodes, node_id->valuestring);
                    if (n) {
                        sim_evt.data.node_idx = n - scenario->nodes->nodes;
                        event_queue_push(scenario->events, &sim_evt);
                    }
                }
            } else if (strcmp(type->valuestring, "interference") == 0) {
                cJSON *cx = cJSON_GetObjectItem(evt, "center_x");
                cJSON *cy = cJSON_GetObjectItem(evt, "center_y");
                cJSON *radius = cJSON_GetObjectItem(evt, "radius");
                cJSON *dur = cJSON_GetObjectItem(evt, "duration_ms");
                
                if (cx && cy && radius && dur) {
                    sim_evt.type = EVT_INTERFERENCE_START;
                    sim_evt.data.interference.center_x = (float)cx->valuedouble;
                    sim_evt.data.interference.center_y = (float)cy->valuedouble;
                    sim_evt.data.interference.radius = (float)radius->valuedouble;
                    event_queue_push(scenario->events, &sim_evt);
                    
                    /* Schedule end event */
                    sim_event_t end_evt = sim_evt;
                    end_evt.type = EVT_INTERFERENCE_END;
                    end_evt.timestamp_us += dur->valueint * 1000ULL;
                    event_queue_push(scenario->events, &end_evt);
                }
            }
        }
    }
    
    /* Schedule metrics tick every 5 seconds */
    for (uint64_t t = 5000000; t < scenario->meta.duration_us; t += 5000000) {
        sim_event_t metrics_evt = {0};
        metrics_evt.timestamp_us = t;
        metrics_evt.type = EVT_METRICS_TICK;
        event_queue_push(scenario->events, &metrics_evt);
    }
    
    return true;
}
```

**Commit:** `git add engine/sim_scenario.c && git commit -m "simulator: implement deterministic scenario loader"`

---

### Task 4.3: Update Makefile for sim_scenario
**Edit:** `simulator/engine/Makefile`

Change:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o cJSON.o
```

To:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o cJSON.o
```

Add:
```makefile
sim_scenario.o: sim_scenario.c sim_scenario.h
	$(CC) $(CFLAGS) -c $<
```

**Commit:** `git add engine/Makefile && git commit -m "simulator: update Makefile for sim_scenario"`

---

## Phase 5: Integrate Bramble Component Code

### Task 5.1: Extend ESP stubs for simulator
**Edit:** `test/stubs/esp_stubs.h`

Add at end (before `#endif`):
```c
/* Simulator extensions */
#ifdef BRAMBLE_SIM
extern uint32_t sim_get_time_ms(void);
#define esp_timer_get_time() (sim_get_time_ms() * 1000ULL)
#else
static inline uint32_t esp_timer_get_time(void) { return 0; }
#endif
```

**Commit:** `git add test/stubs/esp_stubs.h && git commit -m "simulator: extend ESP stubs with time hook"`

---

### Task 5.2: Create main.c skeleton
**File:** `simulator/engine/main.c`
```c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sim_event.h"
#include "sim_node.h"
#include "sim_radio.h"
#include "sim_scenario.h"
#include "sim_emitter.h"
#include "sim_random.h"

/* Include Bramble component implementations */
#define BRAMBLE_SIM
#include "../../test/stubs/esp_stubs.h"
#include "../../components/routing/routing.c"
#include "../../components/routing/discovery.c"
#include "../../components/routing/forwarding.c"
#include "../../components/packet/packet.c"

/* Global simulation state */
static node_array_t g_nodes;
static radio_config_t g_radio;
static event_queue_t g_events;
static pcg32_state_t g_rng;
static uint64_t g_sim_time_us = 0;

/* Simulator time for Bramble components */
uint32_t sim_get_time_ms(void) {
    return (uint32_t)(g_sim_time_us / 1000);
}

static void handle_event(sim_event_t *evt);

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <scenario.json>\n", argv[0]);
        return 1;
    }
    
    /* Initialize */
    node_array_init(&g_nodes);
    event_queue_init(&g_events);
    
    /* Load scenario */
    scenario_t scenario = {
        .nodes = &g_nodes,
        .radio = &g_radio,
        .events = &g_events,
        .rng = &g_rng,
    };
    
    if (!scenario_load_file(argv[1], &scenario)) {
        fprintf(stderr, "Failed to load scenario\n");
        return 1;
    }
    
    fprintf(stderr, "Loaded scenario: %s (%d nodes, %d events)\n",
            scenario.meta.name, g_nodes.count, event_queue_count(&g_events));
    
    /* Event loop */
    sim_event_t evt;
    while (event_queue_pop(&g_events, &evt)) {
        g_sim_time_us = evt.timestamp_us;
        handle_event(&evt);
        
        if (g_sim_time_us >= scenario.meta.duration_us) {
            break;
        }
    }
    
    fprintf(stderr, "Simulation complete\n");
    return 0;
}

static void handle_event(sim_event_t *evt) {
    switch (evt->type) {
        case EVT_NODE_MOVE: {
            sim_node_t *node = node_array_get(&g_nodes, evt->data.node.node_idx);
            if (node) {
                node_move(node, evt->data.node.x, evt->data.node.y);
                emit_node_moved(stdout, g_sim_time_us, node->id, node->x, node->y);
            }
            break;
        }
        
        case EVT_NODE_LEAVE: {
            sim_node_t *node = node_array_get(&g_nodes, evt->data.node_idx);
            if (node) {
                node_deactivate(node);
                emit_node_left(stdout, g_sim_time_us, node->id);
            }
            break;
        }
        
        case EVT_INTERFERENCE_START: {
            int zone = radio_add_interference_zone(&g_radio,
                                                   evt->data.interference.center_x,
                                                   evt->data.interference.center_y,
                                                   evt->data.interference.radius);
            (void)zone; /* Will track zone index later */
            break;
        }
        
        case EVT_INTERFERENCE_END: {
            /* For now, clear all zones (deterministic scenarios have 1 at a time) */
            for (int i = 0; i < g_radio.zone_count; i++) {
                radio_clear_interference_zone(&g_radio, i);
            }
            break;
        }
        
        case EVT_METRICS_TICK: {
            /* Placeholder: will compute real metrics in Phase 6 */
            emit_metrics(stdout, g_sim_time_us, 0.0f, 0.0f, g_nodes.count, 0, 0);
            break;
        }
        
        default:
            /* Other events (SEND_PACKET, etc.) handled in later phases */
            break;
    }
}
```

**Commit:** `git add engine/main.c && git commit -m "simulator: add main.c skeleton with event loop"`

---

### Task 5.3: Update Makefile CFLAGS for BRAMBLE_SIM
**Edit:** `simulator/engine/Makefile`

Change:
```makefile
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -I../../test/stubs -I../../components/routing/include -I../../components/packet/include
```

To:
```makefile
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -DBRAMBLE_SIM -I../../test/stubs -I../../components/routing/include -I../../components/packet/include
```

**Commit:** `git add engine/Makefile && git commit -m "simulator: define BRAMBLE_SIM in CFLAGS"`

---

### Task 5.4: Test build
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/engine
make clean
make
```

**Expected output:** Compilation succeeds, `bramble-sim` binary created

**Commit:** `git commit --allow-empty -m "simulator: verify build succeeds"`

---

### Task 5.5: Create minimal test scenario
**File:** `simulator/scenarios/test-2-node.json`
```json
{
  "name": "test-2-node",
  "mode": "deterministic",
  "duration_ms": 10000,
  "nodes": [
    {"id": "A", "x": 0, "y": 0},
    {"id": "B", "x": 100, "y": 0}
  ],
  "radio": {
    "range": 150,
    "loss_pct": 0,
    "propagation_speed_ms_per_unit": 0.1
  },
  "events": [
    {"at_ms": 1000, "type": "move_node", "node": "B", "x": 200, "y": 0}
  ]
}
```

**Commit:** `git add scenarios/test-2-node.json && git commit -m "simulator: add minimal 2-node test scenario"`

---

### Task 5.6: Test run minimal scenario
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/engine
./bramble-sim ../scenarios/test-2-node.json 2>&1 | head -20
```

**Expected:** Stderr shows "Loaded scenario: test-2-node (2 nodes, ...)", stdout shows JSON events including `node_moved` and `metrics`

**Commit:** `git commit --allow-empty -m "simulator: verify minimal scenario runs"`

---

## Phase 6: Metrics Collector

### Task 6.1: Create sim_metrics.h
**File:** `simulator/engine/sim_metrics.h`
```c
#ifndef SIM_METRICS_H
#define SIM_METRICS_H

#include <stdint.h>

typedef struct {
    uint32_t total_packets;
    uint32_t delivered_packets;
    uint32_t dropped_packets;
    uint64_t total_latency_us;
    uint32_t latency_count;
    uint32_t active_nodes;
} metrics_state_t;

void metrics_init(metrics_state_t *m);
void metrics_record_packet_sent(metrics_state_t *m);
void metrics_record_packet_delivered(metrics_state_t *m, uint64_t latency_us);
void metrics_record_packet_dropped(metrics_state_t *m);
void metrics_update_active_nodes(metrics_state_t *m, int count);
float metrics_delivery_rate(const metrics_state_t *m);
float metrics_avg_latency_ms(const metrics_state_t *m);

#endif /* SIM_METRICS_H */
```

**Commit:** `git add engine/sim_metrics.h && git commit -m "simulator: add metrics collector header"`

---

### Task 6.2: Implement sim_metrics.c
**File:** `simulator/engine/sim_metrics.c`
```c
#include "sim_metrics.h"
#include <string.h>

void metrics_init(metrics_state_t *m) {
    memset(m, 0, sizeof(*m));
}

void metrics_record_packet_sent(metrics_state_t *m) {
    m->total_packets++;
}

void metrics_record_packet_delivered(metrics_state_t *m, uint64_t latency_us) {
    m->delivered_packets++;
    m->total_latency_us += latency_us;
    m->latency_count++;
}

void metrics_record_packet_dropped(metrics_state_t *m) {
    m->dropped_packets++;
}

void metrics_update_active_nodes(metrics_state_t *m, int count) {
    m->active_nodes = count;
}

float metrics_delivery_rate(const metrics_state_t *m) {
    if (m->total_packets == 0) {
        return 0.0f;
    }
    return (float)m->delivered_packets / (float)m->total_packets;
}

float metrics_avg_latency_ms(const metrics_state_t *m) {
    if (m->latency_count == 0) {
        return 0.0f;
    }
    return (float)(m->total_latency_us / m->latency_count) / 1000.0f;
}
```

**Commit:** `git add engine/sim_metrics.c && git commit -m "simulator: implement metrics collector"`

---

### Task 6.3: Update Makefile for sim_metrics
**Edit:** `simulator/engine/Makefile`

Change:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o cJSON.o
```

To:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o sim_metrics.o cJSON.o
```

Add:
```makefile
sim_metrics.o: sim_metrics.c sim_metrics.h
	$(CC) $(CFLAGS) -c $<
```

**Commit:** `git add engine/Makefile && git commit -m "simulator: update Makefile for sim_metrics"`

---

### Task 6.4: Integrate metrics into main.c
**Edit:** `simulator/engine/main.c`

Add include after sim_random.h:
```c
#include "sim_metrics.h"
```

Add global after g_rng:
```c
static metrics_state_t g_metrics;
```

Initialize in main() after event_queue_init:
```c
metrics_init(&g_metrics);
```

Replace EVT_METRICS_TICK case in handle_event:
```c
case EVT_METRICS_TICK: {
    int active = 0;
    for (int i = 0; i < g_nodes.count; i++) {
        if (g_nodes.nodes[i].active) active++;
    }
    metrics_update_active_nodes(&g_metrics, active);
    
    emit_metrics(stdout, g_sim_time_us,
                metrics_delivery_rate(&g_metrics),
                metrics_avg_latency_ms(&g_metrics),
                g_metrics.active_nodes,
                g_metrics.total_packets,
                g_metrics.dropped_packets);
    break;
}
```

**Commit:** `git add engine/main.c && git commit -m "simulator: integrate metrics collector into event loop"`

---

## Phase 7: Anomaly Detector

### Task 7.1: Create sim_anomaly.h
**File:** `simulator/engine/sim_anomaly.h`
```c
#ifndef SIM_ANOMALY_H
#define SIM_ANOMALY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define MAX_ROUTE_FLAP_TRACK 64

typedef struct {
    uint32_t dest_addr;
    uint32_t next_hop;
    uint64_t timestamp_us;
} route_change_t;

typedef struct {
    route_change_t changes[MAX_ROUTE_FLAP_TRACK];
    int count;
} route_flap_tracker_t;

void anomaly_init(route_flap_tracker_t *tracker);
bool anomaly_check_route_flap(route_flap_tracker_t *tracker, uint32_t node_addr, uint32_t dest_addr, uint32_t next_hop, uint64_t now_us, FILE *out);

#endif /* SIM_ANOMALY_H */
```

**Commit:** `git add engine/sim_anomaly.h && git commit -m "simulator: add anomaly detector header"`

---

### Task 7.2: Implement sim_anomaly.c
**File:** `simulator/engine/sim_anomaly.c`
```c
#include "sim_anomaly.h"
#include "sim_emitter.h"
#include <string.h>

#define ROUTE_FLAP_WINDOW_US 2000000 /* 2 seconds */
#define ROUTE_FLAP_THRESHOLD 5

void anomaly_init(route_flap_tracker_t *tracker) {
    tracker->count = 0;
}

bool anomaly_check_route_flap(route_flap_tracker_t *tracker, uint32_t node_addr, uint32_t dest_addr, uint32_t next_hop, uint64_t now_us, FILE *out) {
    /* Count recent changes to this dest */
    int recent_count = 0;
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->changes[i].dest_addr == dest_addr &&
            (now_us - tracker->changes[i].timestamp_us) < ROUTE_FLAP_WINDOW_US) {
            recent_count++;
        }
    }
    
    /* Add current change */
    if (tracker->count < MAX_ROUTE_FLAP_TRACK) {
        tracker->changes[tracker->count].dest_addr = dest_addr;
        tracker->changes[tracker->count].next_hop = next_hop;
        tracker->changes[tracker->count].timestamp_us = now_us;
        tracker->count++;
    }
    
    /* Emit anomaly if threshold exceeded */
    if (recent_count >= ROUTE_FLAP_THRESHOLD) {
        char node_str[16], dest_str[16];
        snprintf(node_str, sizeof(node_str), "%08X", node_addr);
        snprintf(dest_str, sizeof(dest_str), "%08X", dest_addr);
        
        char desc[128];
        snprintf(desc, sizeof(desc), "Route to %s changed %d times in 2s", dest_str, recent_count);
        
        emit_anomaly(out, now_us, "warning", "route_flap", node_str, desc);
        return true;
    }
    
    return false;
}
```

**Commit:** `git add engine/sim_anomaly.c && git commit -m "simulator: implement route flap anomaly detector"`

---

### Task 7.3: Update Makefile for sim_anomaly
**Edit:** `simulator/engine/Makefile`

Change:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o sim_metrics.o cJSON.o
```

To:
```makefile
SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o sim_metrics.o sim_anomaly.o cJSON.o
```

Add:
```makefile
sim_anomaly.o: sim_anomaly.c sim_anomaly.h
	$(CC) $(CFLAGS) -c $<
```

**Commit:** `git add engine/Makefile && git commit -m "simulator: update Makefile for sim_anomaly"`

---

### Task 7.4: Test build again
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/engine
make clean && make
```

**Expected:** Clean build, no errors

**Commit:** `git commit --allow-empty -m "simulator: verify build with all C modules"`

---

## Phase 8: Node.js WebSocket Relay Server

### Task 8.1: Initialize server package.json
**File:** `simulator/server/package.json`
```json
{
  "name": "bramble-sim-server",
  "version": "1.0.0",
  "type": "module",
  "scripts": {
    "dev": "tsx relay.ts",
    "build": "tsc"
  },
  "dependencies": {
    "ws": "^8.18.0"
  },
  "devDependencies": {
    "@types/node": "^22.10.5",
    "@types/ws": "^8.5.13",
    "tsx": "^4.19.2",
    "typescript": "^5.7.2"
  }
}
```

**Commit:** `git add server/package.json && git commit -m "simulator: add server package.json"`

---

### Task 8.2: Create server tsconfig.json
**File:** `simulator/server/tsconfig.json`
```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "ESNext",
    "moduleResolution": "bundler",
    "outDir": "./dist",
    "rootDir": ".",
    "strict": true,
    "esModuleInterop": true,
    "skipLibCheck": true,
    "forceConsistentCasingInFileNames": true
  },
  "include": ["*.ts"]
}
```

**Commit:** `git add server/tsconfig.json && git commit -m "simulator: add server TypeScript config"`

---

### Task 8.3: Implement relay.ts
**File:** `simulator/server/relay.ts`
```typescript
import { WebSocketServer, WebSocket } from 'ws';
import { spawn, ChildProcess } from 'child_process';
import { createServer } from 'http';
import { readFileSync } from 'fs';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PORT = 3000;

const httpServer = createServer((req, res) => {
  // Serve static UI files (built React app)
  const uiPath = resolve(__dirname, '../ui/dist');
  
  let filePath = req.url === '/' ? '/index.html' : req.url!;
  const fullPath = resolve(uiPath, '.' + filePath);
  
  try {
    const content = readFileSync(fullPath);
    const ext = filePath.split('.').pop();
    const contentType: Record<string, string> = {
      'html': 'text/html',
      'js': 'application/javascript',
      'css': 'text/css',
      'json': 'application/json',
      'svg': 'image/svg+xml'
    };
    
    res.writeHead(200, { 'Content-Type': contentType[ext!] || 'text/plain' });
    res.end(content);
  } catch (err) {
    res.writeHead(404);
    res.end('Not found');
  }
});

const wss = new WebSocketServer({ server: httpServer });

wss.on('connection', (ws: WebSocket) => {
  console.log('Client connected');
  
  // Spawn C simulator binary
  const simBinary = resolve(__dirname, '../engine/bramble-sim');
  const scenario = resolve(__dirname, '../scenarios/test-2-node.json');
  
  const proc: ChildProcess = spawn(simBinary, [scenario], {
    stdio: ['ignore', 'pipe', 'pipe']
  });
  
  if (proc.stdout) {
    proc.stdout.on('data', (chunk: Buffer) => {
      const lines = chunk.toString().split('\n').filter(l => l.trim());
      lines.forEach(line => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(line);
        }
      });
    });
  }
  
  if (proc.stderr) {
    proc.stderr.on('data', (chunk: Buffer) => {
      console.error('[sim]', chunk.toString());
    });
  }
  
  proc.on('close', (code) => {
    console.log(`Simulator exited with code ${code}`);
    ws.close();
  });
  
  ws.on('close', () => {
    console.log('Client disconnected');
    proc.kill();
  });
});

httpServer.listen(PORT, () => {
  console.log(`Bramble Simulator running on http://localhost:${PORT}`);
});
```

**Commit:** `git add server/relay.ts && git commit -m "simulator: implement WebSocket relay server"`

---

### Task 8.4: Install server dependencies
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/server
npm install
```

**Expected:** Dependencies installed successfully

**Commit:** `git add server/package-lock.json && git commit -m "simulator: install server dependencies"`

---

## Phase 9: React UI Scaffolding

### Task 9.1: Initialize UI package.json
**File:** `simulator/ui/package.json`
```json
{
  "name": "bramble-sim-ui",
  "version": "1.0.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "tsc && vite build",
    "preview": "vite preview"
  },
  "dependencies": {
    "react": "^18.3.1",
    "react-dom": "^18.3.1"
  },
  "devDependencies": {
    "@types/react": "^18.3.18",
    "@types/react-dom": "^18.3.5",
    "@vitejs/plugin-react": "^4.3.4",
    "typescript": "^5.7.2",
    "vite": "^6.0.7"
  }
}
```

**Commit:** `git add ui/package.json && git commit -m "simulator: add UI package.json"`

---

### Task 9.2: Create vite.config.ts
**File:** `simulator/ui/vite.config.ts`
```typescript
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173
  },
  build: {
    outDir: 'dist'
  }
});
```

**Commit:** `git add ui/vite.config.ts && git commit -m "simulator: add Vite config"`

---

### Task 9.3: Create tsconfig.json for UI
**File:** `simulator/ui/tsconfig.json`
```json
{
  "compilerOptions": {
    "target": "ES2020",
    "useDefineForClassFields": true,
    "lib": ["ES2020", "DOM", "DOM.Iterable"],
    "module": "ESNext",
    "skipLibCheck": true,
    "moduleResolution": "bundler",
    "allowImportingTsExtensions": true,
    "resolveJsonModule": true,
    "isolatedModules": true,
    "noEmit": true,
    "jsx": "react-jsx",
    "strict": true,
    "noUnusedLocals": true,
    "noUnusedParameters": true,
    "noFallthroughCasesInSwitch": true
  },
  "include": ["src"]
}
```

**Commit:** `git add ui/tsconfig.json && git commit -m "simulator: add UI TypeScript config"`

---

### Task 9.4: Create index.html
**File:** `simulator/ui/index.html`
```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Bramble Mesh Simulator</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: system-ui, -apple-system, sans-serif; overflow: hidden; }
  </style>
</head>
<body>
  <div id="root"></div>
  <script type="module" src="/src/main.tsx"></script>
</body>
</html>
```

**Commit:** `git add ui/index.html && git commit -m "simulator: add HTML entry point"`

---

### Task 9.5: Create types.ts
**File:** `simulator/ui/src/types.ts`
```typescript
export interface SimEvent {
  t: number;
  type: string;
  [key: string]: any;
}

export interface Node {
  id: string;
  x: number;
  y: number;
  active: boolean;
}

export interface Link {
  from: string;
  to: string;
  quality: number;
}

export interface Metrics {
  delivery_rate: number;
  avg_latency_ms: number;
  active_nodes: number;
  total_packets: number;
  dropped_packets: number;
}

export interface SimState {
  nodes: Map<string, Node>;
  links: Link[];
  events: SimEvent[];
  metrics: Metrics | null;
  running: boolean;
  currentTime: number;
}
```

**Commit:** `git add ui/src/types.ts && git commit -m "simulator: add TypeScript types"`

---

### Task 9.6: Create useSimulation hook
**File:** `simulator/ui/src/hooks/useSimulation.ts`
```typescript
import { useEffect, useReducer, useRef } from 'react';
import { SimState, SimEvent, Node } from '../types';

type Action = 
  | { type: 'ADD_NODE'; node: Node }
  | { type: 'UPDATE_NODE'; id: string; updates: Partial<Node> }
  | { type: 'ADD_EVENT'; event: SimEvent }
  | { type: 'UPDATE_METRICS'; metrics: any }
  | { type: 'SET_RUNNING'; running: boolean };

function reducer(state: SimState, action: Action): SimState {
  switch (action.type) {
    case 'ADD_NODE': {
      const nodes = new Map(state.nodes);
      nodes.set(action.node.id, action.node);
      return { ...state, nodes };
    }
    case 'UPDATE_NODE': {
      const nodes = new Map(state.nodes);
      const node = nodes.get(action.id);
      if (node) {
        nodes.set(action.id, { ...node, ...action.updates });
      }
      return { ...state, nodes };
    }
    case 'ADD_EVENT':
      return { 
        ...state, 
        events: [...state.events.slice(-99), action.event],
        currentTime: action.event.t 
      };
    case 'UPDATE_METRICS':
      return { ...state, metrics: action.metrics };
    case 'SET_RUNNING':
      return { ...state, running: action.running };
    default:
      return state;
  }
}

const initialState: SimState = {
  nodes: new Map(),
  links: [],
  events: [],
  metrics: null,
  running: false,
  currentTime: 0
};

export function useSimulation() {
  const [state, dispatch] = useReducer(reducer, initialState);
  const ws = useRef<WebSocket | null>(null);

  useEffect(() => {
    const socket = new WebSocket(`ws://${window.location.host}`);
    ws.current = socket;

    socket.onopen = () => {
      console.log('Connected to simulator');
      dispatch({ type: 'SET_RUNNING', running: true });
    };

    socket.onmessage = (event) => {
      try {
        const msg: SimEvent = JSON.parse(event.data);
        
        // Handle different event types
        if (msg.type === 'node_joined' || msg.type === 'node_moved') {
          dispatch({
            type: state.nodes.has(msg.node) ? 'UPDATE_NODE' : 'ADD_NODE',
            ...(state.nodes.has(msg.node) 
              ? { id: msg.node, updates: { x: msg.x, y: msg.y } }
              : { node: { id: msg.node, x: msg.x, y: msg.y, active: true } })
          });
        } else if (msg.type === 'node_left') {
          dispatch({ type: 'UPDATE_NODE', id: msg.node, updates: { active: false } });
        } else if (msg.type === 'metrics') {
          dispatch({ type: 'UPDATE_METRICS', metrics: msg });
        }
        
        dispatch({ type: 'ADD_EVENT', event: msg });
      } catch (err) {
        console.error('Failed to parse event:', err);
      }
    };

    socket.onclose = () => {
      console.log('Disconnected from simulator');
      dispatch({ type: 'SET_RUNNING', running: false });
    };

    return () => socket.close();
  }, []);

  return state;
}
```

**Commit:** `git add ui/src/hooks/useSimulation.ts && git commit -m "simulator: implement useSimulation hook"`

---

### Task 9.7: Create App.tsx skeleton
**File:** `simulator/ui/src/App.tsx`
```typescript
import { useSimulation } from './hooks/useSimulation';
import MeshCanvas from './components/MeshCanvas';
import MetricsDashboard from './components/MetricsDashboard';
import EventLog from './components/EventLog';
import PlaybackControls from './components/PlaybackControls';

export default function App() {
  const state = useSimulation();

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100vh' }}>
      <PlaybackControls running={state.running} currentTime={state.currentTime} />
      
      <div style={{ display: 'flex', flex: 1, overflow: 'hidden' }}>
        <div style={{ flex: 1 }}>
          <MeshCanvas nodes={state.nodes} />
        </div>
        
        <div style={{ width: '300px', borderLeft: '1px solid #ddd' }}>
          <MetricsDashboard metrics={state.metrics} />
        </div>
      </div>
      
      <EventLog events={state.events} />
    </div>
  );
}
```

**Commit:** `git add ui/src/App.tsx && git commit -m "simulator: create App component skeleton"`

---

### Task 9.8: Create main.tsx entry point
**File:** `simulator/ui/src/main.tsx`
```typescript
import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
```

**Commit:** `git add ui/src/main.tsx && git commit -m "simulator: create React entry point"`

---

### Task 9.9: Install UI dependencies
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/ui
npm install
```

**Expected:** Dependencies installed

**Commit:** `git add ui/package-lock.json && git commit -m "simulator: install UI dependencies"`

---

## Phase 10: MeshCanvas Component

### Task 10.1: Implement MeshCanvas.tsx
**File:** `simulator/ui/src/components/MeshCanvas.tsx`
```typescript
import { Node } from '../types';

interface Props {
  nodes: Map<string, Node>;
}

export default function MeshCanvas({ nodes }: Props) {
  const width = 800;
  const height = 600;
  const padding = 50;

  // Auto-scale nodes to fit canvas
  const allNodes = Array.from(nodes.values());
  const maxX = Math.max(...allNodes.map(n => n.x), 100);
  const maxY = Math.max(...allNodes.map(n => n.y), 100);
  
  const scaleX = (x: number) => padding + (x / maxX) * (width - 2 * padding);
  const scaleY = (y: number) => padding + (y / maxY) * (height - 2 * padding);

  return (
    <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', height: '100%', background: '#f5f5f5' }}>
      <svg width={width} height={height} style={{ background: 'white', border: '1px solid #ddd' }}>
        {/* Grid */}
        <defs>
          <pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
            <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#eee" strokeWidth="1" />
          </pattern>
        </defs>
        <rect width={width} height={height} fill="url(#grid)" />
        
        {/* Nodes */}
        {allNodes.map(node => (
          <g key={node.id}>
            <circle
              cx={scaleX(node.x)}
              cy={scaleY(node.y)}
              r={20}
              fill={node.active ? '#4CAF50' : '#ccc'}
              stroke="#333"
              strokeWidth={2}
            />
            <text
              x={scaleX(node.x)}
              y={scaleY(node.y)}
              textAnchor="middle"
              dominantBaseline="middle"
              fill="white"
              fontWeight="bold"
              fontSize="14"
            >
              {node.id}
            </text>
          </g>
        ))}
      </svg>
    </div>
  );
}
```

**Commit:** `git add ui/src/components/MeshCanvas.tsx && git commit -m "simulator: implement MeshCanvas component"`

---

## Phase 11: MetricsDashboard Component

### Task 11.1: Implement MetricsDashboard.tsx
**File:** `simulator/ui/src/components/MetricsDashboard.tsx`
```typescript
import { Metrics } from '../types';

interface Props {
  metrics: Metrics | null;
}

export default function MetricsDashboard({ metrics }: Props) {
  if (!metrics) {
    return (
      <div style={{ padding: '20px' }}>
        <h3>Metrics</h3>
        <p style={{ color: '#999' }}>Waiting for data...</p>
      </div>
    );
  }

  const cards = [
    { label: 'Delivery Rate', value: `${(metrics.delivery_rate * 100).toFixed(1)}%`, color: '#4CAF50' },
    { label: 'Avg Latency', value: `${metrics.avg_latency_ms.toFixed(1)} ms`, color: '#2196F3' },
    { label: 'Active Nodes', value: metrics.active_nodes.toString(), color: '#FF9800' },
    { label: 'Total Packets', value: metrics.total_packets.toString(), color: '#9C27B0' },
    { label: 'Dropped', value: metrics.dropped_packets.toString(), color: '#F44336' }
  ];

  return (
    <div style={{ padding: '20px', overflowY: 'auto' }}>
      <h3 style={{ marginBottom: '16px' }}>Metrics</h3>
      {cards.map(card => (
        <div key={card.label} style={{
          marginBottom: '12px',
          padding: '12px',
          background: '#f9f9f9',
          borderLeft: `4px solid ${card.color}`,
          borderRadius: '4px'
        }}>
          <div style={{ fontSize: '12px', color: '#666', marginBottom: '4px' }}>
            {card.label}
          </div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: card.color }}>
            {card.value}
          </div>
        </div>
      ))}
    </div>
  );
}
```

**Commit:** `git add ui/src/components/MetricsDashboard.tsx && git commit -m "simulator: implement MetricsDashboard component"`

---

## Phase 12: EventLog Component

### Task 12.1: Implement EventLog.tsx
**File:** `simulator/ui/src/components/EventLog.tsx`
```typescript
import { SimEvent } from '../types';
import { useEffect, useRef } from 'react';

interface Props {
  events: SimEvent[];
}

export default function EventLog({ events }: Props) {
  const logRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (logRef.current) {
      logRef.current.scrollTop = logRef.current.scrollHeight;
    }
  }, [events]);

  const formatTime = (us: number) => `${(us / 1000).toFixed(1)}ms`;

  const getEventColor = (type: string) => {
    if (type === 'anomaly') return '#F44336';
    if (type.includes('dropped')) return '#FF9800';
    if (type.includes('route')) return '#2196F3';
    return '#666';
  };

  return (
    <div style={{ height: '200px', borderTop: '1px solid #ddd', background: '#fafafa' }}>
      <div style={{ padding: '8px 12px', background: '#eee', fontWeight: 'bold', fontSize: '14px' }}>
        Event Log
      </div>
      <div ref={logRef} style={{ height: 'calc(100% - 40px)', overflowY: 'auto', padding: '8px' }}>
        {events.map((evt, idx) => (
          <div key={idx} style={{
            fontSize: '12px',
            padding: '4px 8px',
            borderBottom: '1px solid #eee',
            fontFamily: 'monospace'
          }}>
            <span style={{ color: '#999' }}>[{formatTime(evt.t)}]</span>
            {' '}
            <span style={{ color: getEventColor(evt.type), fontWeight: 'bold' }}>
              {evt.type}
            </span>
            {' '}
            <span>{JSON.stringify(evt, null, 0).slice(0, 100)}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
```

**Commit:** `git add ui/src/components/EventLog.tsx && git commit -m "simulator: implement EventLog component"`

---

## Phase 13: PlaybackControls Component

### Task 13.1: Implement PlaybackControls.tsx
**File:** `simulator/ui/src/components/PlaybackControls.tsx`
```typescript
interface Props {
  running: boolean;
  currentTime: number;
}

export default function PlaybackControls({ running, currentTime }: Props) {
  const formatTime = (us: number) => {
    const sec = us / 1000000;
    return `${sec.toFixed(2)}s`;
  };

  return (
    <div style={{
      display: 'flex',
      alignItems: 'center',
      gap: '16px',
      padding: '12px 20px',
      background: '#2c3e50',
      color: 'white',
      borderBottom: '2px solid #1a252f'
    }}>
      <h1 style={{ fontSize: '18px', margin: 0 }}>Bramble Mesh Simulator</h1>
      
      <div style={{ flex: 1 }} />
      
      <div style={{ fontSize: '14px', fontFamily: 'monospace' }}>
        Time: {formatTime(currentTime)}
      </div>
      
      <div style={{
        width: '12px',
        height: '12px',
        borderRadius: '50%',
        background: running ? '#4CAF50' : '#F44336'
      }} />
      
      <span style={{ fontSize: '12px', color: '#bdc3c7' }}>
        {running ? 'Running' : 'Stopped'}
      </span>
    </div>
  );
}
```

**Commit:** `git add ui/src/components/PlaybackControls.tsx && git commit -m "simulator: implement PlaybackControls component"`

---

## Phase 14: ScenarioLoader Component

### Task 14.1: Create placeholder ScenarioLoader.tsx
**File:** `simulator/ui/src/components/ScenarioLoader.tsx`
```typescript
export default function ScenarioLoader() {
  return (
    <div style={{ padding: '12px' }}>
      <select style={{ padding: '6px', fontSize: '14px' }}>
        <option>test-2-node.json</option>
      </select>
      <button style={{ marginLeft: '8px', padding: '6px 12px', fontSize: '14px' }}>
        Load
      </button>
    </div>
  );
}
```

**Note:** Full scenario loading requires server API extension (out of scope for MVP)

**Commit:** `git add ui/src/components/ScenarioLoader.tsx && git commit -m "simulator: add placeholder ScenarioLoader"`

---

## Phase 15: Docker Packaging

### Task 15.1: Create Dockerfile
**File:** `simulator/Dockerfile`
```dockerfile
# Stage 1: Build C simulator
FROM debian:bookworm-slim AS builder-c
RUN apt-get update && apt-get install -y gcc make && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY ../components ./components
COPY ../test ./test
COPY engine ./simulator/engine
RUN cd simulator/engine && make clean && make

# Stage 2: Build React UI
FROM node:22-slim AS builder-ui
WORKDIR /build
COPY ui/package*.json ./ui/
RUN cd ui && npm ci
COPY ui ./ui
RUN cd ui && npm run build

# Stage 3: Runtime
FROM node:22-slim
WORKDIR /app

# Install server deps
COPY server/package*.json ./server/
RUN cd server && npm ci --production

# Copy built artifacts
COPY --from=builder-c /build/simulator/engine/bramble-sim ./engine/
COPY --from=builder-ui /build/ui/dist ./ui/dist
COPY server ./server
COPY scenarios ./scenarios

EXPOSE 3000
CMD ["node", "server/relay.ts"]
```

**Note:** This uses `node` to run TypeScript directly (requires `tsx` in production deps)

**Commit:** `git add Dockerfile && git commit -m "simulator: add Dockerfile (multi-stage)"`

---

### Task 15.2: Fix Dockerfile for tsx runtime
**Edit:** `simulator/Dockerfile`

Change CMD line:
```dockerfile
CMD ["npx", "tsx", "server/relay.ts"]
```

And add tsx to production deps by editing `server/package.json`:

Move `tsx` from `devDependencies` to `dependencies`:
```json
"dependencies": {
  "ws": "^8.18.0",
  "tsx": "^4.19.2"
}
```

**Commit:** `git add Dockerfile server/package.json && git commit -m "simulator: fix Dockerfile to use tsx runtime"`

---

### Task 15.3: Create docker-compose.yml
**File:** `simulator/docker-compose.yml`
```yaml
version: '3.8'

services:
  bramble-sim:
    build:
      context: ..
      dockerfile: simulator/Dockerfile
    ports:
      - "3000:3000"
    volumes:
      - ./scenarios:/app/scenarios:ro
```

**Commit:** `git add docker-compose.yml && git commit -m "simulator: add docker-compose.yml"`

---

## Phase 16: Sample Scenarios

### Task 16.1: Create 3-node-linear.json
**File:** `simulator/scenarios/3-node-linear.json`
```json
{
  "name": "3-node-linear",
  "mode": "deterministic",
  "duration_ms": 30000,
  "nodes": [
    {"id": "A", "x": 0, "y": 0},
    {"id": "B", "x": 100, "y": 0},
    {"id": "C", "x": 200, "y": 0}
  ],
  "radio": {
    "range": 150,
    "loss_pct": 5,
    "propagation_speed_ms_per_unit": 0.1
  },
  "events": [
    {"at_ms": 5000, "type": "move_node", "node": "B", "x": 100, "y": 50},
    {"at_ms": 10000, "type": "move_node", "node": "B", "x": 500, "y": 0},
    {"at_ms": 15000, "type": "move_node", "node": "B", "x": 100, "y": 0}
  ]
}
```

**Commit:** `git add scenarios/3-node-linear.json && git commit -m "simulator: add 3-node-linear scenario"`

---

### Task 16.2: Create 10-node-grid.json
**File:** `simulator/scenarios/10-node-grid.json`
```json
{
  "name": "10-node-grid",
  "mode": "deterministic",
  "duration_ms": 60000,
  "nodes": [
    {"id": "A", "x": 0, "y": 0},
    {"id": "B", "x": 100, "y": 0},
    {"id": "C", "x": 200, "y": 0},
    {"id": "D", "x": 0, "y": 100},
    {"id": "E", "x": 100, "y": 100},
    {"id": "F", "x": 200, "y": 100},
    {"id": "G", "x": 0, "y": 200},
    {"id": "H", "x": 100, "y": 200},
    {"id": "I", "x": 200, "y": 200},
    {"id": "J", "x": 100, "y": 300}
  ],
  "radio": {
    "range": 150,
    "loss_pct": 3,
    "propagation_speed_ms_per_unit": 0.1
  },
  "events": [
    {"at_ms": 10000, "type": "kill_node", "node": "E"},
    {"at_ms": 20000, "type": "interference", "center_x": 100, "center_y": 100, "radius": 80, "duration_ms": 5000},
    {"at_ms": 30000, "type": "move_node", "node": "J", "x": 100, "y": 150}
  ]
}
```

**Commit:** `git add scenarios/10-node-grid.json && git commit -m "simulator: add 10-node-grid scenario"`

---

### Task 16.3: Create stress-test.json (stochastic placeholder)
**File:** `simulator/scenarios/stress-test.json`
```json
{
  "name": "stress-test-stochastic",
  "mode": "stochastic",
  "seed": 42,
  "duration_ms": 60000,
  "nodes": {
    "count": 15,
    "area": [500, 500]
  },
  "radio": {
    "range": 150,
    "loss_pct_range": [2, 15]
  },
  "chaos": {
    "node_churn": {"join_rate_per_min": 2, "leave_rate_per_min": 1},
    "movement": {"speed_max": 10, "pattern": "random_walk"},
    "interference": {"frequency_per_min": 3, "radius_range": [30, 100], "duration_range_ms": [1000, 5000]}
  },
  "traffic": {
    "messages_per_min": 5,
    "random_pairs": true
  }
}
```

**Note:** Stochastic mode not implemented yet (scenario loader will reject)

**Commit:** `git add scenarios/stress-test.json && git commit -m "simulator: add stochastic scenario placeholder"`

---

## Phase 17: End-to-End Integration Test

### Task 17.1: Build React UI for production
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/ui
npm run build
```

**Expected:** `dist/` directory created with compiled assets

**Commit:** `git add -f ui/dist && git commit -m "simulator: build production UI bundle"`

---

### Task 17.2: Test server locally
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator/server
npm run dev
```

**Expected:** Server starts on port 3000, stderr shows "Bramble Simulator running..."

**Verify in another terminal:**
```bash
curl -s http://localhost:3000/ | head -5
```

**Expected:** HTML content with `<title>Bramble Mesh Simulator</title>`

Kill server with Ctrl+C

**Commit:** `git commit --allow-empty -m "simulator: verify server runs locally"`

---

### Task 17.3: Test full Docker build
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator
docker-compose build
```

**Expected:** Build completes successfully with all stages

**Commit:** `git commit --allow-empty -m "simulator: verify Docker build"`

---

### Task 17.4: Run Docker container
```bash
cd /home/justin/.openclaw/workspace/bramble/simulator
docker-compose up
```

**Expected:** Container starts, logs show "Bramble Simulator running on http://localhost:3000"

**Verify:** Open browser to `http://localhost:3000`, see UI with nodes rendering

**Cleanup:**
```bash
docker-compose down
```

**Commit:** `git commit --allow-empty -m "simulator: verify Docker container runs end-to-end"`

---

### Task 17.5: Create integration test script
**File:** `simulator/test-e2e.sh`
```bash
#!/bin/bash
set -e

echo "=== Bramble Simulator E2E Test ==="

echo "1. Building C engine..."
cd engine && make clean && make && cd ..

echo "2. Building UI..."
cd ui && npm run build && cd ..

echo "3. Running test scenario..."
timeout 5s engine/bramble-sim scenarios/test-2-node.json > /tmp/sim-output.json 2>&1 || true

echo "4. Validating output..."
if grep -q '"type":"node_moved"' /tmp/sim-output.json; then
  echo "✓ Found node_moved event"
else
  echo "✗ Missing expected events"
  exit 1
fi

if grep -q '"type":"metrics"' /tmp/sim-output.json; then
  echo "✓ Found metrics event"
else
  echo "✗ Missing metrics"
  exit 1
fi

echo "=== All tests passed ==="
```

Make executable and run:
```bash
chmod +x /home/justin/.openclaw/workspace/bramble/simulator/test-e2e.sh
cd /home/justin/.openclaw/workspace/bramble/simulator
./test-e2e.sh
```

**Expected:** Script completes with "All tests passed"

**Commit:** `git add test-e2e.sh && git commit -m "simulator: add end-to-end integration test script"`

---

## Final Commit

### Task 17.6: Update main README
**File:** `simulator/README.md`
```markdown
# Bramble Mesh Simulator

Network simulator for Bramble that runs actual C component code against a virtual mesh with real-time React visualization.

## Quick Start

### Docker (recommended)
```bash
docker-compose up
```
Open http://localhost:3000

### Local Development
```bash
# Build C engine
cd engine && make

# Build UI
cd ui && npm install && npm run build

# Run server
cd server && npm install && npm run dev
```

## Architecture

- **C Engine** (`engine/`) — Event-driven simulator with Bramble components included at compile time
- **Node.js Server** (`server/`) — WebSocket relay spawning C binary, serves static UI
- **React UI** (`ui/`) — SVG mesh canvas + metrics dashboard + event log

## Scenarios

See `scenarios/*.json` for examples. Format:
- **Deterministic:** Scripted events at specific timestamps
- **Stochastic:** Seeded random chaos events (not yet implemented)

## Testing

```bash
./test-e2e.sh
```

## Design

See `docs/plans/2026-02-16-simulator-design.md` for full specification.
```

**Commit:** `git add README.md && git commit -m "simulator: add README with quick start guide"`

---

### Task 17.7: Final push
```bash
cd /home/justin/.openclaw/workspace/bramble
git log --oneline feature/mesh-simulator | head -20
```

**Expected:** Show commits from this plan

**Verify branch:**
```bash
git branch --show-current
```

**Expected:** `feature/mesh-simulator`

**Push:**
```bash
git push -u origin feature/mesh-simulator
```

**Commit:** `git commit --allow-empty -m "simulator: implementation complete — ready for review"`

---

## Summary

**Implemented:**
- ✅ C event-driven simulation engine with priority queue
- ✅ Seeded PRNG for deterministic runs
- ✅ Virtual node management with per-node Bramble state
- ✅ Distance-based radio propagation model
- ✅ JSON scenario loader (deterministic mode)
- ✅ Integration of actual Bramble routing/forwarding/discovery code
- ✅ Metrics collection (delivery rate, latency, packet counts)
- ✅ Route flap anomaly detector
- ✅ JSON event emitter to stdout
- ✅ Node.js WebSocket relay server
- ✅ React UI with SVG mesh canvas
- ✅ Metrics dashboard
- ✅ Event log with auto-scroll
- ✅ Playback controls (status display)
- ✅ Docker multi-stage build
- ✅ Three sample scenarios
- ✅ E2E integration test

**Not Implemented (out of scope for MVP):**
- Stochastic scenario mode
- Actual packet forwarding logic in event loop (skeleton exists)
- Full anomaly detection suite (only route flap implemented)
- Playback pause/resume/speed control
- Scenario file upload UI
- Animated packet visualization

**Total Tasks:** 103 bite-sized steps (2-5 minutes each)

**Estimated Implementation Time:** 4-6 hours with executing-plans agent

