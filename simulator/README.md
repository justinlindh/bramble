# Bramble Mesh Simulator

Network simulator for Bramble that runs actual C component code against a virtual mesh with real-time React visualization.

## Quick Start

### Docker (recommended)

```bash
cd simulator
docker compose up --build
```

Open <http://localhost:3003>

### Local Development

```bash
# Build Go+C server
cd gosim && go build -o bramble-gosim . && cd ..

# Build UI
cd ui && npm install && npm run build && cd ..

# Run
./gosim/bramble-gosim --ui ui/dist --scenarios scenarios
```

Open <http://localhost:3000>

The local emulator (`emulator/`) also defaults to port 3000, so pass
`--port 3011` (or any free port) if you want to run both at once. The Docker
variants do not collide: this one publishes 3003 and the emulator 3004.

### Headless Mode

```bash
./gosim/bramble-gosim --headless --scenario scenarios/ideal-10-node.json
# Or use the script:
./scripts/run-scenario.sh scenarios/ideal-10-node.json
```

`--no-collisions` disables the collision model (ideal parallel channel) for
baseline comparisons.

## Architecture

Two-tier: **Go+C server** → **React UI**

- **Go Server** (`gosim/`): Simulation engine embedding Bramble C code via cgo, WebSocket hub, HTTP server with REST API and static file serving
- **React UI** (`ui/`): SVG mesh canvas, metrics dashboard, event log, playback controls, scenario loader

The Go server includes all Bramble C components at compile time (same pattern as `test/test_integration.c`). No modifications to Bramble source code.

## Radio model

The shared LoRa medium is modeled in `engine/sim_radio.c`:

- **Real time-on-air.** Every frame occupies the channel for its actual LoRa
  ToA at the configured SF/BW/CR, computed by the firmware's own
  `bramble_calculate_airtime_us` (Semtech AN1200.13). The default PHY mirrors
  the firmware's `RADIO_PROFILE_LONG_RANGE`: SF10, 125 kHz, CR 4/5, 22 dBm.
  That default is only the starting point for an emulator scenario: a real
  firmware node reports the PHY it actually configured on every `tx`, and the
  broker adopts it (`extConn.adoptReportedPHY`) unless the scenario pinned
  `radio.sf`/`radio.bw_hz` itself. The running firmware overwrites the profile's
  SF with the frequency plan's default (SF9/125 kHz on every shipped plan), so
  without that adoption the ether charged roughly twice the true airtime and the
  emulator scenarios' short beacon cadence oversubscribed the channel.
- **Collisions.** Two packets overlapping in time at a receiver, both audible
  there (within the range disk), destroy each other.
- **Capture effect.** The packet at least 6 dB stronger survives an overlap if
  it started first or within the interferer's preamble window (the receiver
  can re-sync during a preamble). The 6 dB co-SF threshold follows Bor,
  Roedig, Voigt, Alonso, "Do LoRa Low-Power Wide-Area Networks Scale?"
  (MSWiM 2016) and SX126x co-channel rejection figures.
- **Half-duplex.** A node cannot receive while transmitting, and its own
  transmissions are serialized (a queued TX starts when the current one ends).
- **Listen-before-talk.** Mirrors `transmit_packet` in `main/mesh_task.c`:
  up to 3 CAD checks, randomized exponential backoff (50 to 300 ms base plus
  an equal random component), then transmit anyway. CAD is modeled as
  deterministic energy detection within the range disk.
- **RSSI.** Log-distance path loss: `RSSI(d) = tx_power - 52 dB - 29 log10(d)`
  with d in grid units (1 unit = 10 m), exponent n = 2.9, 52 dB free-space
  reference loss at 10 m / 915 MHz. RSSI feeds capture comparisons and link
  metrics.
- **Range derives from SF/BW, not a fixed disk.** Deliverability is gated on
  distance vs. `config->range`, but `range` itself is computed from the link
  budget: the distance at which the RSSI gradient above crosses
  `radio_sensitivity_dbm(sf, bw_hz)`, the SX127x/SX126x datasheet sensitivity
  (SF7 -123 dBm ... SF12 -137 dBm at 125 kHz, worsening by
  `10*log10(bw/125000)` dB at wider bandwidth) plus a calibration constant
  (`NOISE_MARGIN_DB` in `sim_radio.c`) chosen so the firmware's default PHY
  (SF10/125 kHz) reproduces the simulator's long-standing ~150-unit baseline
  range under the default path-loss params. Higher SF has more link budget
  and longer range; wider bandwidth raises the noise floor and shortens it.
  A scenario's `radio.range` field, if present, overrides the derivation
  (an escape hatch for topology tests that want range decoupled from
  SF/BW); otherwise range is recomputed from whatever `sf`/`bw_hz`/
  `tx_power_dbm`/`path_loss_*` the scenario configured.

Collision outcomes are evaluated at end-of-packet (delivery time), when every
transmission that could overlap the packet's air window is known. Overlap is
computed on transmit windows; propagation offsets are ignored (microseconds
against ToA of hundreds of milliseconds).

Per-scenario overrides in the `radio` JSON object: `sf`, `bw_hz`, `cr`,
`tx_power_dbm`, `capture_db`, `path_loss_exp`, `collisions` (bool), `lbt`
(bool), plus the existing `range` (explicit override; omit to derive from
`sf`/`bw_hz`), `loss_pct`, `propagation_speed_ms_per_unit`.

The model is validated by unit tests (overlap, capture timing, half-duplex,
TX serialization, LBT) and an ALOHA calibration test that reproduces the
analytic pure-ALOHA collision rate; see `gosim/collision_test.go` and
`gosim/aloha_test.go`. Scale-scenario results live in
`../docs/results/simulation-2026-06.md`.

## Scenarios

Place JSON files in `scenarios/`. Supports:

- **Deterministic:** Scripted events at specific timestamps
- **Stochastic:** Seeded PRNG chaos events (reproducible)
- **Anomaly detection:** Black hole, partition, route loop, excessive RREQ

See `scenarios/` for examples. Upload custom scenarios via the UI or `POST /api/scenarios/upload`.

## Emulator scenarios (real firmware nodes)

A scenario may declare `firmware_nodes`: real Bramble firmware built for the
ESP-IDF linux target (`emulator/node`), spawned as host processes that attach to
the broker over emu-link and participate in the ether like any other node. These
run in real-time mode (wall clock), so durations are seconds, not instant.

```json
"firmware_nodes": [
  { "type": "firmware", "binary": "emulator/node/build/bramble-node.elf",
    "count": 1, "positions": [[0,0]], "label": "sender",
    "env": { "EMU_NETWORK_KEY": "<64 hex chars>", "EMU_AUTO_SEND": "HELLO" } }
]
```

Per-node `env` knobs (host-only, honored only on the linux target):

| Variable | Effect |
| --- | --- |
| `EMU_NETWORK_KEY` | 32-byte network key as 64 hex chars. Seeds provisioning at boot so the fleet meshes (there is no emu-link provisioning RPC). Unset means the node boots INERT. |
| `EMU_AUTO_SEND` | Message text the node originates after a delay (via the real `mesh_send_broadcast`/`mesh_send_message`). The scripted stand-in for a button compose+send. |
| `EMU_AUTO_SEND_TO` | DM target: `neighbor` (first learned neighbor), a hex address, or unset for a channel broadcast. |
| `EMU_AUTO_SEND_DELAY_MS` / `_REPEAT` / `_INTERVAL_MS` | First-phase send timing (default 12000 / 3 / 4000). The delay must exceed the receiver's 10s message-idle threshold so an inbound message auto-opens its Messages screen. |
| `EMU_AUTO_SEND2` + `EMU_AUTO_SEND2_DELAY_MS` / `_REPEAT` / `_INTERVAL_MS` | Optional second send phase (distinct text) after the first, for the DM-desync repro. |
| `EMU_BEACON_INTERVAL_MS` | Overrides the beacon interval (production default 60s) so nodes beacon often enough that neighbor discovery is reliable inside a short scenario window, instead of hinging on a single beacon that can collide or drop. Linux target only. |
| `EMU_DROP_DM_SESSION_AT_MS` | The node tears down its own DM sessions once at this time while staying up and still neighboring the peer, constructing the one-sided-session desync deterministically. What the `emu-dm-desync` scenario uses to avoid reboot's real-time races. |
| `EMU_REBOOT_AT_MS` | The node exits once at this time so the supervisor restarts it (same identity, cleared RAM). A heavier route to the one-sided-session precondition, kept as a general primitive; the desync scenario prefers `EMU_DROP_DM_SESSION_AT_MS`. One-shot via a `NODE_DIR` marker. |

### Assertion vocabulary

`emulator/ci/run_scenarios.sh` runs the emulator scenarios headless and gates CI
on their assertions. Two levels:

- **Screen (OCR-free):** `bramble-gosim screen-assert -log <log> -text <str>`
  with a node selector, one of `-node <hello-id>`, `-at <X,Y>` (the node that
  joined at that position), or `-min-nodes <N>` (at least N distinct nodes). It
  rasterizes `<str>` with the firmware's own `font_6x8` glyphs and blit rule and
  searches every `device_fb` frame for a pixel-exact match (both ink polarities,
  both panel orientations). "The message renders on the pager screen" is an
  assertable fact; a near-miss string is rejected.
- **Log signature:** the runner greps the scenario's event log for a firmware
  console line (e.g. the DM-desync `Failed session decrypt` symptom and the #138
  `re-initiating handshake (self-heal)` recovery), used where a behavior is
  deterministic in the logs but its final on-screen effect has real-time timing
  variance.

Bundled emulator scenarios: `emulator-3-pagers` (attach/persistence smoke),
`emu-channel-delivery` (broadcast renders on both receivers), `emu-dm-desync`
(one-sided DM session desync plus self-heal).

## Interactive Controls

Via the UI or WebSocket (`ws://host:port/ws`):

- Load/start/restart scenarios
- Play/pause with speed control (0.5×–100×)
- **Add nodes**: "+ Node" button, placed near random existing node
- **Move nodes**: drag with mouse or touch
- **Delete nodes**: right-click context menu, or drag to trash zone (bottom-right)
- Inject messages between arbitrary nodes
- Create interference zones

All added/moved nodes are full protocol participants (beaconing, routing, forwarding).

## Design Docs

- Simulator architecture: the "Architecture" section above, plus the inline
  documentation in `gosim/`
- Component architecture: [`../docs/bramble-architecture.md`](../docs/bramble-architecture.md)
- Protocol spec: [`../docs/bramble-protocol-spec.md`](../docs/bramble-protocol-spec.md)
- Anomaly detection: [`../docs/bramble-anomaly-detection.md`](../docs/bramble-anomaly-detection.md)
- Emulator (real firmware binaries on this same ether):
  [`../emulator/README.md`](../emulator/README.md) and `../emulator/DESIGN.md`
