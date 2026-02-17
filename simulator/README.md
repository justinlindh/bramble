# Bramble Mesh Simulator

Network simulator for Bramble that runs actual C component code against a virtual mesh with real-time React visualization.

## Quick Start

### Docker (recommended)
```bash
cd simulator
docker compose up --build
```
Open http://localhost:3000

### Local Development
```bash
# Build C engine
cd engine && make && cd ..

# Install & build UI
cd ui && npm install && npm run build && cd ..

# Install & run server
cd server && npm install && npx tsx relay.ts
```
Open http://localhost:3000

## Architecture

- **C Engine** (`engine/`) — Event-driven simulator with Bramble components included at compile time
- **Node.js Server** (`server/`) — WebSocket relay spawning C binary, serves static UI
- **React UI** (`ui/`) — SVG mesh canvas + metrics dashboard + event log

## Scenarios

Place JSON files in `scenarios/`. Format:
- **Deterministic:** Scripted events at specific timestamps
- **Stochastic:** Seeded random chaos events (not yet implemented)

See `scenarios/` for examples.

## Design

See `docs/plans/2026-02-16-simulator-design.md` for full specification.
