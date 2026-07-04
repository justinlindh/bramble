# Bramble Mesh Simulator

Network simulator for Bramble that runs actual C component code against a virtual mesh with real-time React visualization.

## Quick Start

### Docker (recommended)
```bash
cd simulator
docker compose up --build
```
Open http://localhost:3003

### Local Development
```bash
# Build Go+C server
cd gosim && go build -o bramble-gosim . && cd ..

# Build UI
cd ui && npm install && npm run build && cd ..

# Run
./gosim/bramble-gosim --ui ui/dist --scenarios scenarios
```
Open http://localhost:3000

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

- **Go Server** (`gosim/`) — Simulation engine embedding Bramble C code via cgo, WebSocket hub, HTTP server with REST API and static file serving
- **React UI** (`ui/`) — SVG mesh canvas, metrics dashboard, event log, playback controls, scenario loader

The Go server includes all Bramble C components at compile time (same pattern as `test/test_integration.c`). No modifications to Bramble source code.

## Radio model

The shared LoRa medium is modeled in `engine/sim_radio.c`:

- **Real time-on-air.** Every frame occupies the channel for its actual LoRa
  ToA at the configured SF/BW/CR, computed by the firmware's own
  `bramble_calculate_airtime_us` (Semtech AN1200.13). The default PHY mirrors
  the firmware's `RADIO_PROFILE_LONG_RANGE`: SF10, 125 kHz, CR 4/5, 22 dBm.
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

## Interactive Controls

Via the UI or WebSocket (`ws://host:port/ws`):
- Load/start/restart scenarios
- Play/pause with speed control (0.5×–100×)
- **Add nodes** — "+ Node" button, placed near random existing node
- **Move nodes** — drag with mouse or touch
- **Delete nodes** — right-click context menu, or drag to trash zone (bottom-right)
- Inject messages between arbitrary nodes
- Create interference zones

All added/moved nodes are full protocol participants (beaconing, routing, forwarding).

## Design Docs

- Architecture: `docs/archive/plans/2026-02-16-simulator-design.md`
- Go server design: `docs/archive/plans/2026-02-17-go-simulation-server-design.md`
- Anomaly detection: `docs/bramble-anomaly-detection.md`
