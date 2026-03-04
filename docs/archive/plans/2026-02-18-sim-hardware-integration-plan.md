# Simulator + Real Hardware Integration Plan
*Created: 2026-02-18*

## Overview
Integrate real Heltec V3 boards into the Bramble mesh simulator so virtual and physical nodes coexist in the same simulation. Phased approach from simple to sophisticated.

## Current State
- Simulator: Go+cgo server on port 3003, React UI, Docker container
- Real hardware: 2x Heltec V3 boards on WiFi (192.168.1.64 + 192.168.1.21)
- Both expose JSON-RPC 2.0 over WebSocket at `/ws`
- Simulator nodes use the C engine with `#include`'d Bramble component code

## Architecture Decision
Start with **Message Proxy** (Phase 1) — immediate value, zero firmware changes.
Then add **Packet-Level Radio Tap** (Phase 2) for full protocol fidelity.
**RF Gateway** (Phase 3) deferred until field testing is needed.

---

## Phase 1: Message Proxy
*Estimate: 2 hours | Firmware changes: NONE*

### Concept
Real board appears as a positioned node in the simulator. Messages flow:
- Sim node sends to proxy → proxy calls `bramble.sendMessage` on real board → LoRa TX
- Real board receives LoRa → `bramble.onMessage` notification → proxy injects into sim as received message

### Task 1.1: Hardware node type in Go server
- File: `simulator/gosim/hardware.go` (new)
- `HardwareNode` struct: ws URL, position, address, WebSocket connection
- Implements enough of the sim node interface for the UI to display it
- Auto-connect on sim start, reconnect on disconnect
- Poll `bramble.getStatus` + `bramble.getNeighbors` every 5s for live data

### Task 1.2: WebSocket client for board communication
- File: `simulator/gosim/hardware.go`
- Dial board's WebSocket, subscribe to `bramble.onMessage` notifications
- Parse incoming messages → create sim events (message received)
- Forward sim-routed messages → call `bramble.sendMessage` / `bramble.sendBroadcast`

### Task 1.3: Sim config for hardware nodes
- Extend scenario JSON format:
  ```json
  {
    "hardware_nodes": [
      { "url": "ws://192.168.1.64/ws", "x": 300, "y": 200, "label": "Board 1" },
      { "url": "ws://192.168.1.21/ws", "x": 500, "y": 200, "label": "Board 2" }
    ]
  }
  ```
- Also support runtime add via WebSocket command from UI

### Task 1.4: UI display for hardware nodes
- File: `simulator/ui/src/components/MeshCanvas.tsx`
- Hardware nodes rendered with distinct visual (solid border, antenna icon, pulsing dot)
- Show live RSSI/SNR from board's neighbor table
- Click to view board status (firmware version, uptime, radio config)

### Task 1.5: Bidirectional message flow
- When sim delivers a message TO a hardware node → proxy sends via RPC
- When hardware node receives a message (onMessage) → inject into sim event queue
- Handle broadcasts: hardware broadcast → all sim nodes in range receive
- Map sim addresses to hardware addresses (hardware node keeps its real address)

### Validation
- Start sim with 5 virtual nodes + 2 hardware nodes
- Send message from virtual node → arrives on real board (check `bramble.getMessages`)
- Send broadcast from real board → appears in sim event log
- Screenshot sim UI showing hardware nodes with live data

---

## Phase 2: Packet-Level Radio Tap
*Estimate: 3 hours | Firmware changes: ~80 lines*

### Concept
Real board participates in the simulator's radio medium. Every packet TX'd by the board is captured and injected into the sim's radio model; every packet the sim wants to deliver to the board is injected via RPC. Routing, beacons, and crypto all interact.

### Task 2.1: Firmware — bramble.onTxPacket notification
- File: `main/mesh_task.c`
- After every `radio_transmit()`, emit a JSON-RPC notification:
  ```json
  {"jsonrpc":"2.0","method":"bramble.onTxPacket","params":{"raw":"<hex>","len":N}}
  ```
- Raw packet bytes as hex string
- Gated by a flag (only emit when a client has subscribed)

### Task 2.2: Firmware — bramble.rxPacket RPC method
- File: `main/rpc_methods.c`
- Accepts `{"raw":"<hex>","rssi":-50,"snr":10}` 
- Injects the raw packet into the RX queue as if received from radio
- Allows the simulator to deliver virtual packets to the real board

### Task 2.3: Go server — packet bridge
- File: `simulator/gosim/hardware.go` (extend)
- Subscribe to `bramble.onTxPacket` → inject packet into sim radio medium
- When sim radio model determines a packet reaches the hardware node → call `bramble.rxPacket`
- Apply sim's propagation model (distance, obstacles, packet loss) between virtual and hardware nodes

### Task 2.4: Beacon integration
- Hardware node's real beacons appear in sim → virtual nodes discover it as neighbor
- Virtual node beacons injected into hardware → hardware discovers virtual neighbors
- Routing (RREQ/RREP) works across the virtual/physical boundary

### Validation
- Hardware node sends beacon → virtual nodes add it as neighbor
- Virtual node sends RREQ to hardware node → RREP flows back
- Multi-hop: virtual A → virtual B → hardware C (3-hop route)
- Screenshot sim showing routing paths crossing virtual/physical boundary

---

## Phase 3: Status Overlay (Quick Win)
*Estimate: 1 hour | Firmware changes: NONE*

### Task 3.1: Hardware status panel in sim UI
- File: `simulator/ui/src/components/HardwareOverlay.tsx` (new)
- Floating panel showing live data from connected boards
- Fields: address, firmware version, uptime, peers, beacon TX/RX, RSSI, radio config
- Poll every 5s via `bramble.getStatus` + `bramble.getNeighbors`
- Color-coded health: green (ok), yellow (degraded), red (disconnected)

### Task 3.2: Board connection manager in sim UI
- Add "Connect Hardware" button to sim toolbar
- Input for WebSocket URL + label
- Save board connections in localStorage
- Show connection status (connected/disconnected/reconnecting)

---

## Phase 4: RF Gateway (Future)
*Estimate: 4 hours | Firmware changes: ~100 lines*
*Deferred until field testing is needed.*

### Concept
One board runs in "gateway mode" — promiscuous reception of all LoRa packets in range. Every received packet (even from non-Bramble devices) is forwarded to the simulator. The simulator can also inject packets for the gateway to transmit.

### Task 4.1: Firmware — promiscuous RX mode
- Disable address filtering, forward all received packets via `bramble.onRxPacket`
- Include raw bytes + RSSI + SNR + timestamp
- Separate from normal operation (toggle via `bramble.setGatewayMode`)

### Task 4.2: Go server — RF environment model
- Build physical radio environment model from gateway observations
- Map unknown addresses to "external nodes" in the sim
- Show real-world RF conditions in the UI

---

## Implementation Order
1. Phase 3 (Status Overlay) — quick win, 1 hour, immediate visual feedback
2. Phase 1 (Message Proxy) — core integration, 2 hours
3. Phase 2 (Packet-Level Radio Tap) — full fidelity, after firmware flash
4. Phase 4 (RF Gateway) — when field testing needed

## Files Changed
### Go server (simulator/gosim/)
- `hardware.go` (new) — hardware node management + WebSocket client
- `sim.go` — integrate hardware nodes into simulation loop
- `ws.go` — expose hardware node commands to UI
- `main.go` — hardware node config flags

### React UI (simulator/ui/src/)
- `components/MeshCanvas.tsx` — hardware node rendering
- `components/HardwareOverlay.tsx` (new) — live board status
- `hooks/useSimulation.ts` — hardware node state management

### Firmware (main/)
- `mesh_task.c` — onTxPacket notification (Phase 2 only)
- `rpc_methods.c` — rxPacket method (Phase 2 only)

## Notes
- Hardware nodes use their real addresses (no mapping needed)
- Sim radio propagation model applies to virtual↔hardware paths
- Hardware nodes can be added/removed at runtime without restarting sim
- Docker container needs network access to board IPs (use `network_mode: host` or add board IPs to docker network)
