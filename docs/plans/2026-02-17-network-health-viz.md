# Network Health Visualization

**Date:** 2026-02-17
**Status:** Draft
**Scope:** Companion app — mesh topology, message path visualization, health monitoring

## Summary

Bring the simulator's mesh visualization capabilities into the companion app, powered by real device data. Bramble's per-message `relay_path` in delivery receipts gives us something Meshtastic doesn't have: actual path data for every message, not just occasional debug traceroutes. This plan turns that data into a live, useful network health view.

## Why This Matters

Mesh networks are opaque. Users send a message and either it arrives or it doesn't. They have no idea why delivery takes 2 seconds sometimes and 15 seconds other times, or why messages to a specific node started failing. Network health viz makes the mesh legible — useful for deployment debugging, node placement optimization, and just satisfying curiosity about how your packets travel.

---

## 1. Live Mesh Topology View

### What the user sees
A force-directed graph. Each discovered node is a circle (sized by role: router > relay > endpoint). Links between neighbors are lines with thickness proportional to link quality and color coding:
- **Green:** RSSI > -90 dBm, PDR > 90%
- **Yellow:** RSSI -90 to -110 dBm, PDR 70–90%
- **Red:** RSSI < -110 dBm or PDR < 70%
- **Gray dashed:** node heard but no recent beacon (stale)

The local node is visually distinct (filled, labeled "You"). Nodes are labeled with short name or last 4 of address.

### Data source
Periodic polling via JSON-RPC:
```
bramble.getNeighborTable() → [{ addr, rssi, snr, last_heard_ms, pdr }]
bramble.getRoutingTable() → [{ dest, next_hop, hop_count, metric, age_ms }]
```

The neighbor table gives direct links. The routing table gives inferred multi-hop topology — if we know A→B→C from routing entries, we draw A–B and B–C even though we only directly hear B.

### Layout
- Default: force-directed (d3-force or dagre)
- User can drag nodes to pin them; positions saved to localStorage
- Zoom/pan with touch gestures
- Auto-layout button to reset

### Update cadence
- Poll neighbor + routing tables every 5s (configurable)
- Animate transitions: new nodes fade in, disappearing nodes fade to gray over 30s then remove
- Link quality updates smoothly (color/thickness interpolation)

---

## 2. Message Path Visualization

### Trigger
When a delivery receipt arrives with `relay_path`, the companion app can animate the path.

### Animation
1. Highlight source node (pulse)
2. Animate a particle along each hop: source → relay₁ → relay₂ → … → destination
3. Each hop segment takes ~300ms (tunable)
4. Particle color: green for successful delivery
5. Path segments briefly glow, then fade to a dim trail that persists for ~5s

### Hop-by-hop latency
`relay_path` entries: `{ addr: u16, rssi: i8 }` — 3 bytes per hop, up to 8 hops.

Currently no per-hop timestamp (too expensive in packet space). But we can show:
- **Total RTT:** receipt arrival time − message send time
- **Per-hop RSSI:** annotate each segment with the RSSI recorded at that relay

Future: if we add optional timing (see data architecture), we could show per-hop latency bars.

### Failed deliveries
If no receipt arrives within timeout, show the last known path to that destination (from routing table) with a red "?" at the expected next hop. Helps users see *where* the mesh is broken.

---

## 3. Per-Link Health Indicators

### Neighbor detail panel
Tap a link or long-press a neighbor node to see:

| Metric | Source | Display |
|--------|--------|---------|
| RSSI trend | Last 60 samples from neighbor table polls | Sparkline (60 points, 5s interval = 5 min window) |
| SNR | Neighbor table | Single value + color |
| PDR (packet delivery rate) | Computed on-device from beacon success rate | Percentage + color |
| Last heard | Neighbor table `last_heard_ms` | "12s ago" / "3 min ago" / "LOST" |
| Hop count to dest | Routing table | Number |

### Color thresholds

```
RSSI:  > -80 green, -80 to -100 yellow, < -100 red
SNR:   > 5 green, 0 to 5 yellow, < 0 red
PDR:   > 90% green, 70-90% yellow, < 70% red
Last heard: < 30s green, 30s-2min yellow, > 2min red
```

### Client-side storage
Store RSSI/SNR samples in a circular buffer (IndexedDB). Keep 1 hour of 5s samples = 720 entries per neighbor. Negligible storage.

---

## 4. Node Health Cards

Tap a node to open a detail card showing everything we know about it.

### Data from beacons
Bramble beacons already carry: `battery_pct`, `uptime_s`, `airtime_budget_pct`, `tx_queue_depth`, `fw_version`, `role`.

```
bramble.getNodeInfo(addr) → {
  addr, short_name, role,
  battery_pct, uptime_s,
  airtime_budget_pct, tx_queue_depth,
  fw_version, last_beacon_ms,
  position: { lat, lon } | null
}
```

### Card layout
```
┌─────────────────────────────┐
│  🟢 Node "Relay-West"       │
│  Address: 0x1A3F             │
│  Role: Router                │
├─────────────────────────────┤
│  🔋 Battery    87%  ████░   │
│  ⏱  Uptime     4h 12m       │
│  📡 Airtime    62% remaining │
│  📦 Queue      2 packets     │
│  💾 Firmware   0.4.1         │
│  📍 Last beacon 8s ago       │
├─────────────────────────────┤
│  Links: 3 neighbors          │
│  Routes through: 5 dests     │
│  Relay load: 12 msg/hr       │
└─────────────────────────────┘
```

Bottom section ("Relay load") is computed client-side from observed relay_path data.

---

## 5. Historical Path Analysis

### Storage
IndexedDB table: `message_paths`
```ts
interface MessagePath {
  id: string           // message ID
  dest: number         // destination address
  path: number[]       // [source, relay1, ..., dest]
  rssi_per_hop: number[]
  rtt_ms: number
  timestamp: number    // epoch ms
}
```

Keep last 1000 paths (or 7 days, whichever is less). ~50 bytes per entry = trivial.

### Analysis views

**Per-destination summary:**
> Messages to "Relay-West" (last 24h):
> - Usual path: You → 0x3B → 0x1A (2 hops, avg 340ms) — 85% of messages
> - Alternate: You → 0x7C → 0x5E → 0x1A (3 hops, avg 620ms) — 15%
> - Delivery rate: 94% (32/34)

**Route change detection:**
Compare current path to mode path for each destination. If different, show:
> ⚠ Route to "Relay-West" changed: was 2 hops via 0x3B, now 3 hops via 0x7C→0x5E. RTT increased 340ms → 620ms.

**Trend chart:**
RTT over time per destination. X-axis = time, Y-axis = RTT. Color-code by path taken. Makes route instability visible.

---

## 6. Network-Wide Metrics

### Aggregate dashboard (separate tab or top bar)

| Metric | Computation | Update |
|--------|------------|--------|
| Total nodes seen | Count of unique addrs in neighbor + routing tables | Every poll |
| Active nodes | Beaconed in last 5 min | Every poll |
| Mesh diameter (est.) | Max hop_count in routing table | Every poll |
| Avg delivery rate | Receipts received / messages sent (sliding 1h window) | Per message |
| Busiest relays | Rank nodes by frequency in relay_path data | Per receipt |
| Your airtime usage | From local node metrics | Every poll |

### Bottleneck detection
If >50% of your message paths go through a single relay, highlight it:
> ⚠ Node 0x3B relays 73% of your traffic. If it goes down, most routes will break.

---

## 7. Alerts

### Alert types

| Alert | Trigger | Severity |
|-------|---------|----------|
| Neighbor lost | Was in neighbor table, absent for >2 min | Warning |
| Route broken | Routing table entry removed or metric degraded significantly | Warning |
| Delivery rate drop | Sliding 1h PDR drops below 70% for a destination | Warning |
| Airtime critical | Own or relay node airtime_budget < 10% | Critical |
| New node discovered | Address not seen before appears in neighbor/routing table | Info |

### Delivery
- In-app notification banner (non-blocking)
- Optional browser notification (if companion app has permission)
- Alert history in EventLog panel

### Suppression
- Don't re-alert for same condition within 5 minutes
- User can snooze alerts per-node

---

## 8. Data Architecture

### JSON-RPC endpoints needed

**Already exist (or trivially exposed):**
```
bramble.getNeighborTable() → NeighborEntry[]
bramble.getRoutingTable() → RoutingEntry[]
bramble.getNodeInfo(addr?) → NodeInfo     // addr=null for local node
bramble.getMetrics() → { tx_count, rx_count, airtime_used_ms, ... }
```

**New endpoints needed:**
```
bramble.getDeliveryStats(dest?, window_s?) → {
  sent: number,
  delivered: number,
  avg_rtt_ms: number,
  paths: { path: number[], count: number }[]
}
```

This is optional — can be computed client-side from receipt events instead. Prefer client-side to keep firmware simple.

### Event subscriptions
The companion app already subscribes to events via JSON-RPC notifications:
```
bramble.onDeliveryReceipt → { msg_id, dest, relay_path, rtt_ms }
bramble.onNeighborChange → { added: [], removed: [], updated: [] }
bramble.onBeaconReceived → { addr, beacon_fields }
```

`onNeighborChange` and `onBeaconReceived` may need to be added. Currently the app polls; events would reduce latency and traffic.

### What lives where

| Data | On-device | Client-side |
|------|-----------|-------------|
| Neighbor table | ✅ source of truth | Cached, polled |
| Routing table | ✅ source of truth | Cached, polled |
| Node info / beacons | ✅ latest only | Cached with history |
| RSSI trends | ❌ | ✅ circular buffer in IndexedDB |
| Message paths | ❌ (relay_path in receipt) | ✅ stored in IndexedDB |
| Aggregate stats | ❌ | ✅ computed from local data |
| Alert state | ❌ | ✅ client-side |

Design principle: **keep the node firmware simple**. It exposes raw tables and emits events. All aggregation, trending, analysis, and alerting happens in the companion app.

---

## 9. Reuse from Simulator

### Simulator components (simulator/ui/src/)

| Component | Reusable? | Adaptation needed |
|-----------|-----------|-------------------|
| `MeshCanvas` | ✅ High | Replace simulated node positions with force-layout. Replace sim data source with JSON-RPC polling. Core rendering (Canvas2D, node/link drawing) reusable as-is. |
| `PacketAnimation` | ✅ High | Same animation system, different trigger (receipt events instead of sim packet events). |
| `MetricsDashboard` | ✅ Medium | Metrics are different (sim tracks global perfect-knowledge metrics). Adapt to partial-knowledge metrics from single node's perspective. |
| `EventLog` | ✅ High | Nearly identical. Filter to real events instead of sim events. |
| `TopologyGraph` | ✅ High | If this exists as a separate component from MeshCanvas, direct reuse. |

### Shared component library approach

Extract reusable components to `packages/mesh-viz/`:
```
packages/mesh-viz/
  src/
    MeshCanvas.tsx       — node/link rendering, zoom/pan, click handlers
    PacketAnimation.tsx  — hop-by-hop particle animation
    HealthIndicators.tsx — RSSI sparklines, PDR bars, color coding
    NodeCard.tsx         — node detail card
    types.ts             — shared types (Node, Link, Path, etc.)
```

Both `simulator/ui/` and `companion/` depend on `@bramble/mesh-viz`. Simulator provides simulated data; companion provides real device data. Same visual components.

Use Vite library mode or just a shared workspace package (npm workspaces / pnpm workspaces).

---

## 10. Implementation Plan

### Phase 1: Topology + Message Paths (2–3 weeks)

**Goal:** See your mesh and watch messages travel through it.

Tasks:
- [ ] Extract `MeshCanvas` and `PacketAnimation` from simulator into `packages/mesh-viz/`
- [ ] Define shared types: `MeshNode`, `MeshLink`, `PathTrace`
- [ ] Implement JSON-RPC polling layer in companion app (`useNeighborTable`, `useRoutingTable` hooks)
- [ ] Build force-directed layout using d3-force, with node pinning + localStorage persistence
- [ ] Wire `MeshCanvas` to live polling data
- [ ] Subscribe to delivery receipt events; trigger `PacketAnimation` on relay_path
- [ ] Add failed delivery indicator (red "?" on last known path)
- [ ] Basic styling: node circles with role-based sizing, link lines with RSSI-based color

**Deliverable:** Companion app "Network" tab with live topology and animated message paths.

### Phase 2: Health Indicators + Node Cards (1–2 weeks)

**Goal:** Understand link quality and node status at a glance.

Tasks:
- [ ] Implement RSSI/SNR circular buffer in IndexedDB
- [ ] Build `HealthIndicators` component: sparklines, PDR bars
- [ ] Build `NodeCard` component with beacon-sourced data
- [ ] Add `bramble.onBeaconReceived` event to firmware JSON-RPC interface (if not present)
- [ ] Wire node tap → card display
- [ ] Wire link tap → health panel
- [ ] Color-code topology links by composite health score
- [ ] Add "last heard" aging: nodes/links fade as they go stale

**Deliverable:** Tap any node or link to see health details. Visual link quality on topology.

### Phase 3: Historical Analysis + Alerts (2 weeks)

**Goal:** See patterns over time, get notified when things go wrong.

Tasks:
- [ ] Implement `message_paths` IndexedDB table with retention policy
- [ ] Build per-destination path summary view
- [ ] Implement route change detection (compare current vs. modal path)
- [ ] Build RTT trend chart (simple canvas sparkline or lightweight chart lib)
- [ ] Implement network-wide metrics dashboard
- [ ] Implement bottleneck detection
- [ ] Build alert system: triggers, notification banner, history log, snooze
- [ ] Add `bramble.onNeighborChange` event to firmware if not present
- [ ] Wire alerts to neighbor loss, route break, PDR drop, airtime critical

**Deliverable:** "History" sub-tab with path analysis. Alert banner + log. Network metrics summary.

### Phase 4: Polish + Optimization (1 week)

- [ ] Performance: throttle canvas redraws, virtualize event log
- [ ] Responsive layout for mobile (BLE use case) vs. desktop (serial)
- [ ] Dark/light theme support matching companion app
- [ ] Export: download topology snapshot as PNG, export path data as CSV
- [ ] Documentation: user-facing guide for network health features

---

## Open Questions

1. **Position data:** If nodes have GPS, should topology view use real geographic positions instead of force-layout? Could offer a toggle: "logical" vs. "geographic" view.
2. **Multi-node perspective:** Currently this is single-node (your node's view of the mesh). If you have serial access to multiple nodes, could we show a merged view? Probably Phase 5.
3. **Airtime visualization:** Could show an "airtime budget" bar for the mesh — how close are we to regulatory duty cycle limits? Useful for dense deployments.
4. **Relay_path compression:** Currently 3 bytes/hop × 8 hops max = 24 bytes. If we want per-hop timestamps, that's +2 bytes/hop (relative ms, u16). Worth the packet overhead? Probably optional/configurable.
