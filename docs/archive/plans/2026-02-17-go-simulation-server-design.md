# Go Simulation Server — Design Document

**Date:** 2026-02-17
**Branch:** `feature/mesh-simulator`
**Status:** Design

## Overview

Rewrite the Bramble mesh simulator from a three-tier architecture (C engine → Node.js relay → React UI) to a two-tier architecture (Go server with embedded C → React UI). The Go server replaces both the C `main.c` event loop and the Node.js WebSocket relay, providing a real-time interactive simulation that accepts commands mid-run.

## Motivation

The current architecture has fundamental limitations:

1. **OOM risk** — The C engine runs to completion, emitting all events to stdout. The Node.js relay buffers every event in memory before playback starts. Large simulations (hundreds of nodes, long durations) exhaust memory.
2. **No interactivity** — The engine is fire-and-forget. You can't add nodes, inject messages, or change conditions mid-simulation. The "Add Node" button currently fakes it with a visual-only event.
3. **Unnecessary complexity** — Three separate runtimes (C binary, Node.js, React) with IPC plumbing between them. Two of those can be one binary.

## Architecture

```
┌─────────────────────────────────────────────┐
│                   Browser                    │
│              React UI (unchanged)            │
│         WebSocket + HTTP static files        │
└──────────────────┬──────────────────────────┘
                   │ ws://host:3003
┌──────────────────┴──────────────────────────┐
│              Go Server Binary                │
│                                              │
│  ┌──────────┐  ┌───────────┐  ┌──────────┐  │
│  │ WebSocket│  │ Sim Loop  │  │ Scenario │  │
│  │ Handler  │←→│ (goroutine│←→│  Loader  │  │
│  │          │  │  + ticker)│  │  (cJSON)  │  │
│  └──────────┘  └─────┬─────┘  └──────────┘  │
│                      │ cgo                    │
│  ┌───────────────────┴──────────────────┐    │
│  │           libsim (C)                  │    │
│  │  Bramble components + sim modules     │    │
│  │  (routing, forwarding, packet,        │    │
│  │   radio, nodes, events, anomaly,      │    │
│  │   metrics, emitter, random, scenario) │    │
│  └───────────────────────────────────────┘    │
└──────────────────────────────────────────────┘
```

### What stays the same
- **Bramble core C code** — `components/routing/`, `components/packet/` — zero modifications
- **Simulator C modules** — `sim_node`, `sim_radio`, `sim_event`, `sim_random`, `sim_metrics`, `sim_anomaly`, `sim_emitter`, `sim_scenario` — reused as-is via cgo
- **React UI** — same components, same WebSocket protocol, same event types
- **Scenario JSON format** — fully backward compatible
- **Docker deployment** — still a single container on port 3003

### What changes
- **Node.js relay (`relay.ts`)** → deleted, replaced by Go server
- **C `main.c`** → deleted, replaced by Go simulation loop calling into C via cgo
- **Playback model** → real-time simulation with speed control, not record-then-replay

## C Library Interface (`libsim`)

The existing simulator C modules are compiled into a static library via cgo. The Go code calls into C through a thin API layer. Rather than creating a new `libsim.c` wrapper, Go calls the existing C functions directly — the headers already provide a clean API.

### C functions called from Go

**Scenario loading:**
```c
// sim_scenario.h
bool scenario_load_file(const char *path, scenario_t *scenario);
```

**Node management:**
```c
// sim_node.h
void node_array_init(node_array_t *array);
int  node_array_add(node_array_t *array, const char *id, uint32_t addr, float x, float y);
sim_node_t *node_array_find_by_id(node_array_t *array, const char *id);
sim_node_t *node_array_find_by_addr(node_array_t *array, uint32_t addr);
void node_activate(sim_node_t *node);
void node_deactivate(sim_node_t *node);
void node_move(sim_node_t *node, float x, float y);
void node_tick(sim_node_t *node, uint64_t now_us, node_tick_result_t *result);
```

**Event queue:**
```c
// sim_event.h
void event_queue_init(event_queue_t *queue);
bool event_queue_push(event_queue_t *queue, const sim_event_t *event);
bool event_queue_pop(event_queue_t *queue, sim_event_t *out);
sim_event_t *event_queue_peek(event_queue_t *queue);
int  event_queue_count(const event_queue_t *queue);
```

**Radio:**
```c
// sim_radio.h
void radio_config_init(radio_config_t *config);
void sim_radio_broadcast(sim_node_t *tx, const outbound_packet_t *pkt,
     node_array_t *nodes, radio_config_t *radio, pcg32_state_t *rng,
     event_queue_t *events, metrics_state_t *metrics, uint64_t now_us);
int  radio_add_interference_zone(radio_config_t *config, float cx, float cy, float r);
void radio_clear_interference_zone(radio_config_t *config, int index);
```

**Metrics & anomaly:**
```c
// sim_metrics.h
void metrics_init(metrics_state_t *metrics);
void metrics_record_packet_sent(metrics_state_t *metrics);
// ... etc

// sim_anomaly.h
void anomaly_init(node_anomaly_tracker_t *t);
void anomaly_check_partition(node_array_t *nodes, float range, uint64_t now_us, FILE *out);
// ... etc
```

**Emitter:**
```c
// sim_emitter.h — all emit_* functions write JSON to a FILE*
void emit_node_joined(FILE *out, uint64_t ts, const char *id, uint32_t addr, float x, float y);
void emit_packet_sent_typed(FILE *out, ...);
// ... etc
```

### The emitter problem

The current emitter functions write directly to `FILE *stdout`. In the Go server, we need events routed to Go (for WebSocket broadcast) instead of stdout.

**Solution: pipe-based capture.** The Go server creates an `os.Pipe()`, and the C code writes to that pipe's `FILE*` instead of stdout. A Go goroutine reads the pipe, parses JSON lines, and dispatches them to WebSocket clients. This requires a one-line change to the emitter calls (pass the pipe's `FILE*` instead of `stdout`), which the Go simulation loop already controls since it's the one calling into C.

Alternatively, we could refactor emitters to write to a ring buffer instead of `FILE*`, but the pipe approach is simpler and avoids changing any C module signatures.

### Bramble `#include .c` pattern

The current `main.c` includes Bramble component `.c` files directly:
```c
#include "../../components/routing/routing.c"
#include "../../components/routing/discovery.c"
#include "../../components/routing/forwarding.c"
#include "../../components/packet/packet.c"
```

For cgo, we need a different approach. Options:

1. **Compile everything into one C compilation unit** — Create a single `all.c` that `#include`s all the `.c` files (Bramble components + sim modules). cgo compiles this as one unit, resolving all symbols.
2. **Build a static library** — Use a Makefile/script to compile all C sources into `libsim.a`, then link via cgo `#cgo LDFLAGS`.

**Choice: Option 1** (single compilation unit). It matches the existing pattern, avoids a separate build step, and cgo handles it natively. The file is just includes:

```c
// simulator/gosim/csrc/all.c
#include "../../../test/stubs/esp_stubs.h"
#include "../engine/sim_event.c"
#include "../engine/sim_random.c"
#include "../engine/sim_emitter.c"
#include "../engine/sim_node.c"
#include "../engine/sim_radio.c"
#include "../engine/sim_scenario.c"
#include "../engine/sim_metrics.c"
#include "../engine/sim_anomaly.c"
#include "../engine/cJSON.c"
#include "../../../components/routing/routing.c"
#include "../../../components/routing/discovery.c"
#include "../../../components/routing/forwarding.c"
#include "../../../components/packet/packet.c"
```

## Go Server Design

### Directory structure

```
simulator/
  engine/           ← existing C sim modules (unchanged)
  gosim/
    main.go         ← entry point, HTTP server, static files
    sim.go          ← simulation state machine + event loop
    bridge.go       ← cgo declarations + Go↔C interface
    ws.go           ← WebSocket hub (broadcast to clients)
    commands.go     ← command parsing (add_node, send_message, etc.)
    csrc/
      all.c         ← single compilation unit (#includes everything)
      bridge.c      ← C helper functions called from Go (event dispatch, etc.)
      bridge.h      ← header for bridge.c
  scenarios/        ← existing JSON scenarios (unchanged)
  ui/               ← existing React app (unchanged)
  scripts/          ← existing headless runner (may need update)
  Dockerfile        ← updated for Go build
  docker-compose.yml
```

### Simulation state machine

```
              load scenario
  IDLE ─────────────────────► LOADED
                                 │
                            play │
                                 ▼
                              RUNNING ◄──── resume
                               │ │
                         pause │ │ complete / duration reached
                               ▼ ▼
                             PAUSED ──── COMPLETED
                               │              │
                          restart              │ restart
                               └───► IDLE ◄───┘
```

States:
- **IDLE** — No scenario loaded. Server is waiting.
- **LOADED** — Scenario parsed, nodes created, topology sent to UI. Waiting for play.
- **RUNNING** — Simulation clock advancing. Events processed and streamed to clients.
- **PAUSED** — Clock frozen. Commands still accepted (add node, etc. — queued at current sim time).
- **COMPLETED** — Duration reached or no more events. Final metrics sent.

### Simulation loop (goroutine)

```go
func (s *Sim) run() {
    ticker := time.NewTicker(time.Millisecond) // 1ms base tick
    defer ticker.Stop()

    for {
        select {
        case <-s.stopCh:
            return

        case cmd := <-s.cmdCh:
            s.handleCommand(cmd)

        case <-ticker.C:
            if s.state != Running {
                continue
            }

            // Advance sim clock based on wall time and speed multiplier
            wallElapsed := time.Since(s.wallStart)
            simNow := s.simTimeAtStart + uint64(wallElapsed.Microseconds()) * uint64(s.speed)

            // Process all events up to simNow
            for {
                evt := C.event_queue_peek(&s.events)
                if evt == nil || uint64(evt.timestamp_us) > simNow {
                    break
                }
                C.event_queue_pop(&s.events, &s.currentEvent)
                s.simTime = uint64(s.currentEvent.timestamp_us)
                s.dispatchEvent(&s.currentEvent)
            }

            // Check duration
            if s.simTime >= s.duration {
                s.complete()
            }
        }
    }
}
```

### Speed control

The simulation uses **wall-clock-relative time**:

- `speed = 1.0` → real-time (1 sim-second = 1 wall-second)
- `speed = 10.0` → 10x fast-forward
- `speed = 100.0` → 100x fast-forward
- `speed = 0` (or special "instant") → process all remaining events immediately, no ticker wait

When speed changes mid-run, re-anchor:
```go
simElapsed := wallElapsed * oldSpeed
s.simTimeAtStart += simElapsed
s.wallStart = time.Now()
s.speed = newSpeed
```

### Command channel

Commands from WebSocket clients are sent to the simulation goroutine via a buffered Go channel:

```go
type Command struct {
    Type    string          // "play", "pause", "speed", "add_node", "send_message", etc.
    Payload json.RawMessage // type-specific data
}
```

This is the key architectural improvement: **commands are processed in the simulation goroutine**, so there are no race conditions on simulation state. The sim loop processes commands between event batches.

### Interactive commands

| Command | Effect |
|---------|--------|
| `play` | Transition LOADED/PAUSED → RUNNING |
| `pause` | RUNNING → PAUSED |
| `restart` | Any → reload scenario, LOADED |
| `speed` | Change speed multiplier (0.5–1000) |
| `instant` | Process all remaining events immediately |
| `add_node` | Create node in C state, schedule tick, emit `node_joined` |
| `remove_node` | Deactivate node, emit `node_left` |
| `move_node` | Update position, emit `node_moved` |
| `send_message` | Inject `EVT_GENERATE_MESSAGE` at current sim time |
| `interference` | Add/remove interference zone |
| `load` | Load new scenario file, → LOADED |

#### `add_node` in detail

This is the command Justin specifically wants to work:

```go
func (s *Sim) cmdAddNode(id string, x, y float32) {
    // Allocate next address
    addr := s.nextAddr
    s.nextAddr++

    // Add to C node array
    cID := C.CString(id)
    defer C.free(unsafe.Pointer(cID))
    C.node_array_add(&s.nodes, cID, C.uint32_t(addr), C.float(x), C.float(y))

    // Initialize anomaly tracker for new node
    idx := s.nodes.count - 1
    C.anomaly_init(&s.anomaly[idx])

    // Emit node_joined event to all clients
    s.emit(NodeJoinedEvent{...})

    // Schedule first tick for this node
    s.scheduleNodeTick(id, s.simTime + 100000) // 100ms from now

    // The node will beacon on its first tick, neighbors will discover it,
    // routes will form naturally through the Bramble protocol.
    // No special-casing needed — the protocol handles it.
}
```

The node **participates fully**: it beacons, responds to RREQs, forwards packets, builds routes. It's a real node in the simulation, not a visual ghost.

### Event output

The current C emitter functions write JSON to `FILE*`. Two approaches to capture this in Go:

#### Approach A: Pipe capture (simpler, chosen)

```go
// Create a pipe — C writes to the write end, Go reads from the read end
r, w, _ := os.Pipe()
s.emitFile = C.fdopen(C.int(w.Fd()), C.CString("w"))

// Reader goroutine
go func() {
    scanner := bufio.NewScanner(r)
    for scanner.Scan() {
        s.hub.Broadcast(scanner.Bytes())
    }
}()
```

All existing `emit_*` calls pass `s.emitFile` instead of `stdout`. The Go reader goroutine parses each JSON line and fans it out to connected WebSocket clients.

#### Approach B: Callback (more complex, future option)

Replace `FILE*` emitters with a callback function pointer that Go sets via cgo. More efficient (no serialization/parsing round-trip) but requires changing all emitter signatures. Not worth it for v1.

### Event filtering

The current relay filters out BEACON and RREQ `packet_sent`/`packet_received` events to reduce noise. The Go server applies the same filter before broadcasting:

```go
func shouldBroadcast(eventType, pktType string) bool {
    if eventType == "packet_sent" || eventType == "packet_received" {
        return pktType != "BEACON" && pktType != "RREQ"
    }
    return true
}
```

### WebSocket hub

Standard pub/sub pattern:

```go
type Hub struct {
    clients    map[*Client]bool
    broadcast  chan []byte
    register   chan *Client
    unregister chan *Client
}
```

Multiple browser tabs can connect simultaneously. All receive the same event stream. Commands from any client go into the same command channel.

### HTTP server

Go's `net/http` serves:
- `/` → React UI static files (embedded or from disk)
- `/api/scenarios` → list available scenarios
- `/api/scenarios/upload` → upload new scenario JSON
- `/ws` → WebSocket upgrade
- `/*` → SPA fallback to `index.html`

### Headless mode

For the `run-scenario.sh` script, the Go binary supports a `--headless` flag:

```bash
bramble-gosim --headless --scenario scenarios/ideal-10-node.json
```

In headless mode:
- No HTTP server or WebSocket
- Speed = instant (process all events immediately)
- Events written to stdout (same JSON lines format)
- Exit on completion with metrics summary to stderr

This preserves backward compatibility with the existing headless runner.

## Event dispatch (Go → C → Go)

The Go simulation loop replaces `main.c`'s `handle_event` function. Each event type maps to C function calls:

```go
func (s *Sim) dispatchEvent(evt *C.sim_event_t) {
    switch evt._type {
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
```

Each handler calls into C for protocol logic (same as current `main.c`), but event *emission* goes through the pipe to Go, which broadcasts to WebSocket clients.

### Packet handling example

`handleReceivePacket` in Go does the same thing as the current C code:

1. Find the receiving node via `C.node_array_find_by_addr()`
2. Deserialize the header via `C.bramble_header_deserialize()`
3. Branch on packet type (BEACON, RREQ, RREP, RERR, DATA)
4. Call appropriate C protocol functions
5. Events emitted by C code flow through the pipe to clients

The protocol logic stays in C. Go is just the orchestrator.

## C bridge helpers (`bridge.c`)

Some operations are awkward to express purely through cgo (unions, complex pointer arithmetic). A thin C bridge file provides helper functions:

```c
// bridge.h

// Access event union fields (cgo struggles with C unions)
void bridge_get_node_event(const sim_event_t *evt, char *node_id, uint32_t *addr, float *x, float *y);
void bridge_get_packet_event(const sim_event_t *evt, uint32_t *src, uint32_t *dest,
                             int8_t *rssi, uint8_t *data, uint16_t *len);
void bridge_get_tick_event(const sim_event_t *evt, char *node_id, uint32_t *tick_seq);
void bridge_get_interference_event(const sim_event_t *evt, int *zone_idx,
                                    float *cx, float *cy, float *radius);

// Set sim time (used by Bramble's esp_timer_stub)
void bridge_set_sim_time(uint64_t time_us);

// High-level packet handlers (wraps the complex switch logic from main.c)
// These call protocol functions and emit events via the provided FILE*
void bridge_handle_beacon(sim_node_t *rx, const uint8_t *buf, uint16_t len,
                          int8_t rssi, uint64_t now_us,
                          node_array_t *nodes, radio_config_t *radio,
                          pcg32_state_t *rng, event_queue_t *events,
                          metrics_state_t *metrics,
                          node_anomaly_tracker_t *anomaly, FILE *out);

void bridge_handle_rreq(/* similar */);
void bridge_handle_rrep(/* similar */);
void bridge_handle_rerr(/* similar */);
void bridge_handle_data(/* similar */);
void bridge_handle_generate_message(/* similar */);
```

This keeps the complex packet handling logic in C (where it naturally belongs, alongside the Bramble protocol code) and gives Go clean function signatures to call.

## Docker build

```dockerfile
# Stage 1: Build UI
FROM node:22-alpine AS ui-build
WORKDIR /app/simulator/ui
COPY simulator/ui/package*.json .
RUN npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 2: Build Go server (with embedded C)
FROM golang:1.24-bookworm AS go-build
RUN apt-get update && apt-get install -y gcc libc6-dev
WORKDIR /app
COPY components/ components/
COPY test/stubs/ test/stubs/
COPY simulator/engine/ simulator/engine/
COPY simulator/gosim/ simulator/gosim/
WORKDIR /app/simulator/gosim
RUN CGO_ENABLED=1 go build -o /bramble-gosim .

# Stage 3: Runtime
FROM debian:bookworm-slim
COPY --from=go-build /bramble-gosim /usr/local/bin/bramble-gosim
COPY --from=ui-build /app/simulator/ui/dist /ui
COPY simulator/scenarios /scenarios
EXPOSE 3000
CMD ["bramble-gosim", "--ui", "/ui", "--scenarios", "/scenarios"]
```

Single binary, small runtime image. No Node.js needed.

## UI changes

**None required.** The React UI connects via WebSocket and processes JSON events. The event format is identical. The WebSocket message protocol for commands (`play`, `pause`, `speed`, `start`, `add_node`) is identical.

The only new capability the UI might want to expose later:
- "Send Message" button (inject a message between two nodes)
- "Add Interference" click-on-canvas
- "Instant Complete" button

These are additive and can be done after the server refactor.

## Migration path

1. Build the Go server alongside the existing C engine + Node.js relay
2. Verify identical behavior with existing scenarios
3. Delete `simulator/server/relay.ts` and C `main.c`
4. Update `Dockerfile` and `docker-compose.yml`
5. Update `scripts/run-scenario.sh` to use `--headless` mode

The existing C sim modules (`sim_event.c`, `sim_node.c`, etc.) are **not deleted** — they're compiled into the Go binary via cgo.

## Performance considerations

- **cgo call overhead**: ~100ns per call. The sim loop calls C functions thousands of times per sim-second. At 100x speed, that's maybe 100K cgo calls/sec = ~10ms overhead. Negligible.
- **Pipe I/O for events**: Each event is a JSON line (~100-500 bytes). At peak (150-node mesh, 5s beacon interval), maybe 1000 events/sec. Pipe throughput is orders of magnitude above this.
- **Event queue size**: Still `MAX_EVENT_QUEUE = 100000` in C. For very large/long simulations, this could be a bottleneck. Future: dynamic allocation or spill-to-disk. Not needed for v1.
- **Memory**: The simulation state (nodes, event queue, metrics, anomaly trackers) is all in C-allocated memory on the heap. Go's GC doesn't touch it. Total: ~50MB for a 256-node sim with full event queue. Fine.

## Testing strategy

- **Headless comparison**: Run each scenario in both old (C engine) and new (Go server `--headless`) mode. Compare final metrics (packets, delivery rate, latency). They should match exactly for the same seed.
- **WebSocket protocol**: Connect a test client, verify event stream matches expected format.
- **Interactive commands**: Script a sequence (load → play → add_node → pause → resume → complete) and verify the added node participates (routes, forwards, receives).
- **OOM regression**: Run a 256-node, 10-minute scenario and verify memory stays bounded.

## Out of scope (future)

- **Multiple concurrent simulations** — Currently one sim per server instance. Could add session IDs later.
- **Scenario editor in UI** — Drag nodes, draw interference zones, export JSON.
- **Persistent replay** — Save event stream to file for later replay without re-running.
- **Distributed simulation** — Multiple Go servers simulating different mesh regions.

## Summary

| Aspect | Before | After |
|--------|--------|-------|
| Architecture | C binary → Node.js relay → React | Go+C binary → React |
| Processes | 3 (C, Node, browser) | 2 (Go, browser) |
| Interactivity | None (replay only) | Full (add/remove nodes, inject messages, change speed) |
| Memory model | Buffer all events in JS heap | Stream events via pipe, process in real-time |
| Speed control | Client-side drain timer | Server-side sim clock with multiplier |
| Docker image | Node.js + C toolchain | Go + small runtime |
| Added node | Visual ghost | Full protocol participant |
