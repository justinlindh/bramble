# Bramble SDK & CLI Ecosystem Design

**Date:** 2026-02-17
**Status:** Approved

---

## 1. Overview & Goals

Bramble is evolving from a single firmware repo into an ecosystem of reusable components. The goal is to make it simple for third-party tools and applications to interact with Bramble mesh nodes, starting with a Go SDK and CLI tool.

**Core principle:** The JSON-RPC 2.0 protocol is the product. The firmware is the reference implementation. The OpenAPI spec is the machine-readable contract. Everything else — SDKs, CLIs, TUIs, third-party integrations — is generated from or built against that contract.

**Goals:**
- Well-defined, versioned API between host software and Bramble nodes
- Go SDK as first client library, importable by any Go application
- CLI tool for interactive and scripted node management
- Architecture that makes future language SDKs easy (via OpenAPI codegen)
- Independent semantic versioning per repository with protocol version negotiation

---

## 2. Repository Structure

### 2.1 bramble (existing)

The firmware repository gains two new things:

- **`api/openapi.yaml`** — The canonical OpenAPI 3.1 spec defining all JSON-RPC methods, request/response schemas, and notification types. This is the source of truth that SDK repos consume.
- **`components/rpc/`** — Transport-agnostic JSON-RPC 2.0 dispatcher in the firmware. Single point where all RPC methods are registered and dispatched.

Everything else stays as-is: firmware, simulator, webapp, docs, components.

**Versioning:** Firmware follows its own semver. The `api/openapi.yaml` includes a `protocol_version` field (e.g., `"0.1.0"`) that is independent of the firmware version. Protocol version bumps follow semver rules based on API changes.

### 2.2 bramble-go (new)

Go module providing a client library for communicating with Bramble nodes.

```
bramble-go/
├── api/                    # Generated types from OpenAPI spec
│   └── types.go            # Structs for all request/response/notification payloads
├── transport/              # Transport implementations
│   ├── transport.go        # Transport interface
│   ├── serial.go           # UART serial (JSON-RPC over newline-delimited JSON)
│   ├── websocket.go        # WiFi WebSocket
│   └── ble.go              # BLE GATT (stub/future)
├── client.go               # High-level client: Connect(), Send(), Peers(), etc.
├── client_options.go       # Functional options pattern
├── protocol.go             # JSON-RPC 2.0 framing, request/response matching
├── version.go              # Protocol version negotiation
├── go.mod
├── go.sum
├── README.md
└── Makefile                # generate target pulls openapi.yaml, runs codegen
```

**Key design points:**
- `Transport` interface abstracts serial/WebSocket/BLE — client code doesn't care
- Auto-reconnect with backoff built into transports
- Notification subscription via callbacks or channels
- Protocol version check on connect — warns or errors if firmware is incompatible
- Zero external dependencies beyond `golang.org/x/` and standard library where possible

**Versioning:** Independent semver. `v0.x` during development. Documents which protocol versions it supports.

### 2.3 bramble-cli (new)

CLI tool built on bramble-go.

```
bramble-cli/
├── cmd/
│   └── bramble/
│       └── main.go         # Entry point
├── internal/
│   ├── commands/            # One file per command group
│   │   ├── send.go
│   │   ├── status.go
│   │   ├── peers.go
│   │   ├── routes.go
│   │   ├── config.go
│   │   ├── channels.go
│   │   ├── location.go
│   │   ├── probe.go
│   │   └── monitor.go      # Live event stream (messages, neighbors, etc.)
│   ├── output/              # Output formatters
│   │   ├── table.go
│   │   ├── json.go
│   │   └── format.go
│   └── discovery/           # Auto-detect nodes on USB, mDNS, etc.
│       └── discover.go
├── go.mod                   # depends on bramble-go
├── go.sum
├── README.md
└── Makefile
```

**Usage examples:**
```bash
# Connect via serial (auto-detect)
bramble status
bramble peers
bramble send 6EEA8967 "hello from the CLI"
bramble broadcast "anyone out there?"
bramble monitor              # live stream of events

# Connect via WiFi
bramble --transport ws://192.168.4.1/ws status

# Explicit serial port
bramble --port /dev/ttyUSB0 peers

# Output formats
bramble peers --json
bramble routes --json | jq '.[] | select(.hop_count > 2)'

# Multi-node (future)
bramble --port /dev/ttyUSB0 --alias node1 status
bramble --port /dev/ttyUSB1 --alias node2 status
```

**CLI framework:** `cobra` — standard for Go CLIs, good completion support, subcommand structure.

**Versioning:** Independent semver. Tracks which bramble-go version it depends on.

---

## 3. Firmware RPC Component

### 3.1 Architecture

New component: `components/rpc/`

```
components/rpc/
├── include/
│   ├── rpc_dispatcher.h    # Public API
│   └── rpc_methods.h       # Method registration declarations
├── rpc_dispatcher.c         # JSON parse, method lookup, dispatch, response formatting
├── rpc_methods.c            # All bramble.* method handlers
├── CMakeLists.txt
└── Kconfig                  # Enable/disable RPC, buffer sizes
```

**Design:**
- `rpc_dispatcher_init()` registers all method handlers in a static table
- `rpc_dispatch(const char *json_in, char *json_out, size_t out_len)` — parse, dispatch, format response. Transport-agnostic.
- Method handlers have signature: `rpc_err_t handler(const cJSON *params, cJSON *result)`
- Uses cJSON (already in ESP-IDF) for parsing/generation
- Notification emission: `rpc_notify(const char *method, cJSON *params)` — queues to all registered transports

### 3.2 Transport Integration

**UART (serial):**
- The existing CLI task reads lines from UART
- Auto-detect: if line starts with `{`, route to `rpc_dispatch()`. Otherwise, route to existing console parser.
- JSON-RPC responses written back to UART as newline-delimited JSON
- Notifications pushed to UART as newline-delimited JSON

**WebSocket (WiFi):**
- ESP-IDF `esp_http_server` with WebSocket upgrade
- Each WebSocket frame is a JSON-RPC message
- Routes to same `rpc_dispatch()`
- Notifications pushed to all connected WebSocket clients

**BLE (future):**
- GATT characteristic for JSON-RPC
- Same `rpc_dispatch()`, chunked for BLE MTU
- Deferred — not in MVP

### 3.3 UART Auto-Detection

```c
void cli_process_line(const char *line) {
    if (line[0] == '{') {
        // JSON-RPC mode
        char response[RPC_MAX_RESPONSE];
        rpc_dispatch(line, response, sizeof(response));
        printf("%s\n", response);
    } else {
        // Human console mode (existing)
        esp_console_run(line, &ret);
    }
}
```

Simple, no mode switching, no configuration. Both protocols coexist on the same UART.

---

## 4. OpenAPI Specification

### 4.1 Location

`bramble/api/openapi.yaml` — lives in the firmware repo because the firmware is the reference implementation. When a method is added to `rpc_methods.c`, the spec is updated in the same commit.

### 4.2 Mapping JSON-RPC to OpenAPI

JSON-RPC methods map to OpenAPI paths:

```yaml
paths:
  /rpc/bramble.getStatus:
    post:
      operationId: getStatus
      requestBody: ...    # JSON-RPC params schema
      responses:
        200: ...          # JSON-RPC result schema
```

This is a convention for documenting JSON-RPC via OpenAPI. The actual wire format remains JSON-RPC 2.0 — the OpenAPI spec is for documentation and codegen, not for REST endpoints.

### 4.3 Methods (v0.1.0 Protocol)

**Query methods:**
| Method | Description |
|--------|-------------|
| `bramble.getStatus` | Node identity, uptime, firmware version, protocol version |
| `bramble.getIdentity` | Public key + address (lightweight) |
| `bramble.getVersion` | Firmware version, protocol version, hardware info |
| `bramble.getConfig` | Radio settings, channels, node name |
| `bramble.getNeighbors` | Neighbor table with RSSI/SNR/last_seen |
| `bramble.getRoutes` | Routing table entries |
| `bramble.getMessages` | Message history (with pagination params) |
| `bramble.getAirtime` | Airtime budget stats |
| `bramble.getPeerLocations` | Location data for known peers |

**Action methods:**
| Method | Description |
|--------|-------------|
| `bramble.sendMessage` | Send text to peer (DM) or broadcast |
| `bramble.sendProbe` | Broadcast delivery probe |
| `bramble.setRadio` | Update radio config (freq, SF, BW, power) |
| `bramble.setNodeName` | Rename node |
| `bramble.addChannel` | Add encrypted channel |
| `bramble.removeChannel` | Remove channel |
| `bramble.setDefaultChannel` | Set default TX channel |
| `bramble.setMailbox` | Enable/disable store-and-forward |
| `bramble.setLocationConfig` | GPS settings (enable, update interval) |
| `bramble.setLocationContact` | Set location sharing tier for a peer |
| `bramble.removeLocationContact` | Remove location sharing for a peer |
| `bramble.shareLocationOnce` | One-shot location share to a peer |
| `bramble.ping` | Connectivity check / keepalive |
| `bramble.reboot` | Restart the node |

**Notifications (node → client):**
| Method | Description |
|--------|-------------|
| `bramble.onMessage` | Incoming message received |
| `bramble.onAck` | Delivery receipt for sent message |
| `bramble.onNeighborChange` | Neighbor table updated |
| `bramble.onRouteChange` | Routing table updated |
| `bramble.onProbeResult` | Probe response received |

---

## 5. Protocol Versioning

The firmware reports its protocol version via `bramble.getVersion`:

```json
{
  "firmware_version": "0.3.0",
  "protocol_version": "0.1.0",
  "hardware": "heltec_v3"
}
```

**Rules:**
- **Patch** (0.1.x): Bug fixes in existing method behavior, no schema changes
- **Minor** (0.x.0): New methods added, existing methods unchanged (backward compatible)
- **Major** (x.0.0): Breaking changes to existing method params/responses

The SDK stores a `min_protocol`/`max_protocol` range it supports. On connect, it calls `bramble.getVersion` and checks compatibility. If the firmware's protocol version is outside the SDK's range, it returns an error with a clear message.

---

## 6. Phased Implementation Plan

### Phase 1: Firmware RPC Foundation
- Create `components/rpc/` with dispatcher + cJSON parsing
- Implement core query methods: getStatus, getIdentity, getVersion, getNeighbors, getRoutes, getAirtime, ping
- Wire UART auto-detection (JSON vs console)
- Unit tests for dispatcher + each method handler
- **Deliverable:** Can send JSON-RPC over serial and get responses alongside existing console

### Phase 2: OpenAPI Spec + Codegen Pipeline + Versioning Docs
- Write `api/openapi.yaml` covering all Phase 1 methods
- Set up codegen tooling (oapi-codegen for Go types)
- Validate spec with OpenAPI linter
- Write `VERSIONING.md` in bramble repo: protocol version lifecycle, semver rules, compatibility matrix, repo relationship diagram
- Each repo's README references VERSIONING.md
- **Deliverable:** Machine-readable API spec, generated Go types, versioning documentation

### Phase 3: bramble-go SDK Core
- Create bramble-go repo on Gitea
- Implement Transport interface + serial transport
- Implement JSON-RPC 2.0 protocol layer (request/response matching, timeout, error handling)
- Implement Client with query methods (Status, Identity, Version, Neighbors, Routes, Airtime, Ping)
- Protocol version negotiation on connect
- Unit tests with mock transport
- **Deliverable:** `go get gitea.example.com/bramble-go` works, can talk to a node over serial

### Phase 4: bramble-cli MVP
- Create bramble-cli repo on Gitea
- Implement: `bramble status`, `bramble peers`, `bramble routes`, `bramble ping`
- USB auto-detection (scan /dev/ttyUSB*)
- Table + JSON output formatters
- **Deliverable:** Working CLI that queries real hardware over USB serial

### Phase 5: Firmware Action Methods + Notifications
- Add action methods to RPC dispatcher: sendMessage, sendProbe, setRadio, setNodeName, channel management, location, mailbox, reboot
- Implement notification emission: onMessage, onAck, onNeighborChange, onRouteChange, onProbeResult
- Register notification transports (UART push, later WebSocket push)
- Update OpenAPI spec
- Unit tests
- **Deliverable:** Full method coverage on firmware side

### Phase 6: SDK + CLI Action Methods
- Add all action methods to bramble-go client
- Add notification subscription (callbacks + Go channels)
- CLI commands: `bramble send`, `bramble broadcast`, `bramble config`, `bramble channels`, `bramble location`, `bramble probe`, `bramble monitor` (live event stream)
- Update bramble-go to match full OpenAPI spec
- **Deliverable:** Full CLI with all commands, live monitoring

### Phase 7: WiFi WebSocket Transport
- Firmware: ESP-IDF HTTP server + WebSocket upgrade, routes to same RPC dispatcher
- Firmware: WiFi station mode (connect to home network) + AP fallback
- bramble-go: WebSocket transport implementation
- CLI: `--transport ws://...` flag, mDNS discovery
- **Deliverable:** CLI works over WiFi, same commands as serial

### Phase 8: Polish & Extended Methods
- Add getIdentity, getVersion, ping, reboot to firmware + SDK + CLI
- Protocol version enforcement in SDK
- Auto-reconnect with backoff in transports
- CLI: shell completion (bash/zsh/fish), man pages
- CLI: `bramble monitor` with filters (--messages, --neighbors, --routes)
- README, examples, contributing guide for each repo
- **Deliverable:** Production-quality SDK and CLI

---

## 7. Future (Not in Scope)

- **Bubbletea TUI** — Rich terminal UI built on bramble-go, separate phase
- **BLE transport** — Third transport, same architecture, deferred
- **Multi-language SDKs** — Generated from OpenAPI spec (Python, TypeScript, Rust)
- **Multi-node CLI** — Connect to multiple nodes simultaneously
- **Plugin system** — Third-party CLI extensions
