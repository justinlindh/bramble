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

## Architecture

Two-tier: **Go+C server** → **React UI**

- **Go Server** (`gosim/`) — Simulation engine embedding Bramble C code via cgo, WebSocket hub, HTTP server with REST API and static file serving
- **React UI** (`ui/`) — SVG mesh canvas, metrics dashboard, event log, playback controls, scenario loader

The Go server includes all Bramble C components at compile time (same pattern as `test/test_integration.c`). No modifications to Bramble source code.

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
