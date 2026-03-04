# Bramble Mesh Simulator — Design Document

**Date:** 2026-02-16
**Status:** Approved

## Overview

A network simulator for Bramble that runs the actual C component code against a virtual mesh with configurable topology, chaos events, and real-world conditions. Includes a real-time React web visualizer served from a single Docker container.

## Goals

- Test Bramble's routing, forwarding, reliability, and crypto code without hardware
- Simulate real-world conditions: packet loss, node churn, movement, interference
- Provide deterministic (scripted) and stochastic (seeded random) scenario modes
- Visualize mesh state, packet flow, metrics, and anomalies in real time
- Zero modifications to existing Bramble component code

## Architecture

Three components in one Docker container:

### 1. Simulation Engine (C)

Compiles Bramble component `.c` files directly (same pattern as `test/test_integration.c`). Contains:

- **Virtual node array** — Each node owns its own `routing_table_t`, `neighbor_table_t`, `pending_discovery_table_t`, `rreq_dedup_t`, etc.
- **Mock radio layer** — Distance-based propagation. RSSI derived from distance. Configurable packet loss. Interference zones.
- **Discrete event queue** — Priority queue sorted by simulation timestamp. Event types:
  - `SEND_PACKET`, `RECEIVE_PACKET`, `TIMER_FIRE`
  - `NODE_JOIN`, `NODE_LEAVE`, `NODE_MOVE`
  - `INTERFERENCE_START`, `INTERFERENCE_END`
  - `GENERATE_MESSAGE`
- **Scenario loader** — Parses JSON scenario files with cJSON. Seeds event queue.
- **Stochastic engine** — Seeded PRNG. Generates random events from chaos parameters. Same seed = identical run.
- **Event emitter** — Writes JSON lines to stdout for every interesting event (packet sent/received/dropped, route change, anomaly, metrics).
- **Metrics collector** — Tracks and periodically emits:
  - Message delivery rate (%)
  - End-to-end latency
  - Route convergence time
  - Control packet overhead ratio
  - Hop count distribution
  - Airtime usage per node
- **Anomaly detector** — Flags and emits events for:
  - Route flaps (repeated route changes to same dest)
  - Black holes (forwarding node, packets never arrive)
  - Mesh partitions (split brain)
  - Excessive retransmissions
  - Route loops
  - Delivery timeouts

### 2. WebSocket Relay Server (Node.js/TypeScript)

- Spawns C simulator binary as child process
- Pipes stdout JSON events to connected WebSocket clients
- Serves built React app as static files
- Handles playback control commands from browser:
  - Pause/resume (SIGSTOP/SIGCONT or custom protocol)
  - Speed control (buffering/throttling event stream)
  - Step forward
- Exposes on port 3000

### 3. React Visualizer (TypeScript + Vite)

#### Components

- **`MeshCanvas`** — SVG-based mesh topology view
  - Nodes as labeled circles
  - Links color-coded by quality (green → yellow → red)
  - Animated dots for packets in flight
  - Interference zones as pulsing translucent red overlays
  - Dead nodes grayed/dashed
  - Tooltips with node stats on hover

- **`MetricsDashboard`** — Right sidebar
  - Cards for each metric with sparkline trend charts
  - Updates on each metrics summary event

- **`EventLog`** — Bottom panel
  - Scrollable event list with timestamp, type icon, description
  - Anomalies highlighted red/orange
  - Filterable by node, event type, severity
  - Auto-scroll with pin toggle

- **`PlaybackControls`** — Top bar
  - Play/pause, speed slider (0.5x–100x), step forward
  - Progress bar (simulation time vs duration)
  - Seed display for stochastic runs

- **`ScenarioLoader`** — Scenario selector/file upload + run button

#### State Management

- `useSimulation` hook — single WebSocket connection, dispatches events to a reducer
- All components consume shared state tree

## Scenario File Format

### Deterministic

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
    {"at_ms": 1000, "type": "send_message", "from": "A", "to": "C", "payload": "hello"},
    {"at_ms": 5000, "type": "move_node", "node": "B", "x": 500, "y": 0},
    {"at_ms": 10000, "type": "join", "node": "D", "x": 100, "y": 50},
    {"at_ms": 15000, "type": "kill_node", "node": "B"},
    {"at_ms": 20000, "type": "interference", "center_x": 100, "center_y": 0, "radius": 80, "duration_ms": 3000}
  ]
}
```

### Stochastic

```json
{
  "name": "stress-test-15-nodes",
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

## Project Structure

```
bramble/simulator/
├── Dockerfile              # Multi-stage: C build → React build → runtime
├── docker-compose.yml
├── scenarios/
│   ├── 3-node-linear.json
│   ├── 10-node-grid-churn.json
│   └── stochastic-stress.json
├── engine/
│   ├── Makefile
│   ├── main.c              # Entry point, event loop, scenario loader
│   ├── sim_node.c/h        # Virtual node management
│   ├── sim_radio.c/h       # Distance-based propagation model
│   ├── sim_event.c/h       # Event queue (priority queue)
│   ├── sim_metrics.c/h     # Metrics collection & emission
│   ├── sim_anomaly.c/h     # Anomaly detection
│   ├── sim_emitter.c/h     # JSON line event output
│   ├── sim_random.c/h      # Seeded PRNG for stochastic mode
│   └── cJSON/              # Vendored single-file JSON library
├── server/
│   ├── package.json
│   ├── relay.ts            # Spawns C binary, WebSocket relay
│   └── tsconfig.json
└── ui/
    ├── package.json
    ├── vite.config.ts
    ├── index.html
    └── src/
        ├── App.tsx
        ├── hooks/useSimulation.ts
        ├── components/
        │   ├── MeshCanvas.tsx
        │   ├── MetricsDashboard.tsx
        │   ├── EventLog.tsx
        │   ├── PlaybackControls.tsx
        │   └── ScenarioLoader.tsx
        └── types.ts
```

## Separation of Concerns

- **No modifications** to any file under `components/`, `main/`, or `test/`
- Simulator `#include`s component `.c` files at compile time (same as existing test pattern)
- All simulation-specific code lives under `simulator/`
- If compile-time hooks are ever needed, they use `#ifdef BRAMBLE_SIM` guards — but the current design avoids this entirely

## Docker

Single container, multi-stage build:

1. **Stage 1 (builder-c):** Debian + gcc + make → compile C simulator binary
2. **Stage 2 (builder-ui):** Node.js → `npm run build` for React app
3. **Stage 3 (runtime):** Node.js slim + compiled C binary + built frontend assets

```yaml
# docker-compose.yml
services:
  bramble-sim:
    build: ./simulator
    ports:
      - "3000:3000"
    volumes:
      - ./simulator/scenarios:/scenarios
```

## Event Protocol (C → Server → Browser)

JSON lines, one event per line:

```json
{"t": 1500, "type": "packet_sent", "from": "A", "to": "B", "packet_type": "RREQ", "id": 42}
{"t": 1502, "type": "packet_received", "at": "B", "from": "A", "packet_type": "RREQ", "id": 42, "rssi": -65}
{"t": 1502, "type": "packet_dropped", "at": "C", "from": "A", "reason": "out_of_range"}
{"t": 1510, "type": "route_added", "node": "B", "dest": "A", "next_hop": "A", "metric": 10}
{"t": 5000, "type": "node_moved", "node": "B", "x": 500, "y": 0}
{"t": 5000, "type": "link_broken", "between": ["A", "B"], "reason": "out_of_range"}
{"t": 10000, "type": "metrics", "delivery_rate": 0.85, "avg_latency_ms": 45, "active_nodes": 4}
{"t": 10000, "type": "anomaly", "severity": "warning", "kind": "route_flap", "node": "C", "dest": "A", "desc": "Route to A changed 5 times in 2s"}
```

## Development Workflow

- Use OpenClaw browser tool to render and verify UI during development
- Send screenshots to Justin via Signal for feedback during iteration
- Scenario files are volume-mounted — edit without rebuilding

## Future Extensions (Out of Scope)

- Hardware-in-the-loop (mix real and simulated nodes)
- 3D terrain/elevation model for radio propagation
- Record/replay of real mesh traffic
- CI integration (run scenarios, assert metrics thresholds)
