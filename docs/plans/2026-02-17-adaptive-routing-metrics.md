# Adaptive Routing Metrics

> ✅ **SIMULATOR IMPLEMENTATION COMPLETE** (2026-02-17) — Checkboxes below not updated but all tasks were implemented in the simulator branch.

**Date:** 2026-02-17
**Status:** Draft
**Area:** `components/routing/`, `components/airtime/`

## Summary

Replace the single-factor RSSI/SNR link penalty metric with a composite metric that accounts for link quality, observed delivery rate, neighbor airtime budget, latency, and hop count. The goal: routes that actually work reliably get preferred over routes that merely have strong signal.

## Current State

Route metric is `255 - compute_link_penalty(rssi, snr)`, range 0–255 (higher = better). This captures instantaneous RF conditions but ignores:

- Whether the neighbor actually delivers packets (asymmetric links, interference)
- Whether the neighbor has airtime budget left to forward
- End-to-end latency through the route
- Congestion at the next hop

A node with RSSI -80 dBm but 95% delivery rate is better than one at -60 dBm with 40% delivery rate. Current metric can't express this.

---

## 1. Multi-Factor Composite Metric

### Formula

```
composite = clamp8(
    w_link * link_quality +
    w_dlvr * delivery_score +
    w_air  * airtime_score +
    w_lat  * latency_score -
    w_hop  * hop_count
)
```

All sub-scores normalized to 0–255. Output clamped to 0–255 (uint8_t, fits existing route table).

### Default Weights

| Factor | Weight | Rationale |
|--------|--------|-----------|
| `w_link` | 0.30 | RF quality still matters, but no longer dominant |
| `w_dlvr` | 0.35 | Delivery rate is the strongest predictor of usable routes |
| `w_air`  | 0.15 | Avoid routing through airtime-starved nodes |
| `w_lat`  | 0.10 | Tiebreaker; latency varies less on LoRa than WiFi |
| `w_hop`  | 0.10 | Prefer shorter paths, all else equal |

Weights stored in `routing_config_t`, configurable at build time via menuconfig. Internally represented as uint8_t percentages summing to 100 to avoid float math.

### Integer Arithmetic

Fixed-point with scale factor 100:

```c
uint16_t raw = (30 * link_quality + 35 * delivery_score +
                15 * airtime_score + 10 * latency_score) / 100;
raw = (raw > 255 * hop_count_penalty) ? raw - 255 * hop_count_penalty : 0;
// hop_count_penalty = (10 * hop_count * 255) / (100 * MAX_HOPS)
uint8_t composite = (raw > 255) ? 255 : (uint8_t)raw;
```

No floating point. No division by non-constants (compiler optimizes `/100`).

---

## 2. Delivery Rate Tracking

### Data Structure

Per-neighbor, in the existing neighbor table:

```c
typedef struct {
    uint16_t tx_count;       // packets sent to this neighbor (saturates at 65535)
    uint16_t ack_count;      // ACKs received
    uint8_t  delivery_ema;   // EMA of delivery rate, 0-255 (255 = 100%)
    uint8_t  sample_count;   // capped at 255, for confidence gating
} neighbor_delivery_t;
```

### EMA Update

On every TX outcome (ACK received or timeout):

```c
// α = 1/8 for smooth tracking (shift-friendly)
// success = 255 on ACK, 0 on timeout
ema = ema - (ema >> 3) + (success >> 3);
```

Alpha of 1/8 means ~8 samples for 63% convergence. After 24 samples the EMA is reliable.

### Confidence Gating

If `sample_count < 8`, use `delivery_score = link_quality` (fall back to RF-only). This prevents new neighbors from getting unfairly penalized or rewarded before we have data.

### Decay

If no packets sent to a neighbor for 5 minutes (configurable), decay:

```c
// Called from beacon timer, every 30s
if (now - neighbor->last_tx_time > DELIVERY_DECAY_INTERVAL) {
    neighbor->delivery.delivery_ema =
        (neighbor->delivery.delivery_ema * 7) >> 3;  // decay toward 0
    // Don't decay below link_quality / 2 — stale isn't broken
    if (neighbor->delivery.delivery_ema < link_quality >> 1)
        neighbor->delivery.delivery_ema = link_quality >> 1;
}
```

### Counter Saturation

When `tx_count` hits 65535, halve both counters:

```c
if (tx_count == UINT16_MAX) {
    tx_count >>= 1;
    ack_count >>= 1;
}
```

---

## 3. Airtime-Aware Routing

### Beacon Extension

Add `airtime_remaining_pct` (uint8_t, 0–100) to beacon payload. This is the percentage of the node's hourly airtime budget remaining.

Computed from `airtime_budget.c`:

```c
uint8_t airtime_remaining_pct(void) {
    return (uint8_t)((budget->tokens_remaining * 100) / budget->tokens_max);
}
```

### Airtime Score

```c
// Linear: 0% remaining → score 0, 100% remaining → score 255
uint8_t airtime_score = (neighbor->airtime_remaining_pct * 255) / 100;
```

### Interaction with Tier Priorities

- **Critical tier** traffic (emergency, route control) ignores airtime score entirely — these packets use the critical budget (36s/hr) and route purely on link+delivery.
- **Normal tier** applies the full composite metric including airtime weighting.
- A node at 0% normal budget but with critical budget remaining is still viable for critical traffic. The airtime score only reflects normal budget for normal-tier route decisions.

### Penalty Cliff

Below 10% airtime remaining, apply a steep penalty multiplier:

```c
if (airtime_remaining_pct < 10) {
    airtime_score = airtime_score >> 2;  // quarter the score
}
```

This makes near-depleted nodes strongly avoided without completely blacklisting them (they're still usable if no alternative exists).

---

## 4. Latency Estimation

### Measurement

When a DATA packet is sent and a delivery receipt returns, compute RTT:

```c
uint32_t rtt_ms = receipt_timestamp - data_send_timestamp;
```

This is end-to-end through the route, not per-hop. Store per route entry (dest_addr).

### Per-Route Latency EMA

```c
// In route_entry_t:
uint16_t latency_ema_ms;  // EMA of RTT in ms, 0 = unknown

// Update (α = 1/4 for faster response to congestion):
latency_ema_ms = latency_ema_ms - (latency_ema_ms >> 2) + (rtt_ms >> 2);
```

### Latency Score

Normalize against expected max latency (configurable, default 10s for multi-hop LoRa):

```c
uint8_t latency_score;
if (latency_ema_ms == 0) {
    latency_score = 128;  // unknown → neutral
} else if (latency_ema_ms >= MAX_EXPECTED_LATENCY_MS) {
    latency_score = 0;
} else {
    latency_score = 255 - (uint8_t)((latency_ema_ms * 255) / MAX_EXPECTED_LATENCY_MS);
}
```

### Limitations

- Only measurable for routes we actively use (no latency data for idle routes)
- Receipt-based: only works for messages that get delivery receipts
- Asymmetric latency not captured (RTT/2 is an approximation)

These are acceptable. Latency is weighted at 10% — it's a tiebreaker, not a primary signal.

---

## 5. Route Quality Signaling in RREPs

### Decision: Advertise per-hop metric, not composite

RREPs already carry the route metric. Change: each intermediate node that forwards an RREP adds its own per-hop composite metric to the accumulated route metric, rather than just the link penalty.

```c
// In RREP forwarding (discovery.c):
// OLD:
rrep->metric += compute_link_penalty(rssi, snr);
// NEW:
rrep->metric += (255 - compute_composite_metric(neighbor, route));
```

The accumulated metric in the RREP represents the sum of per-hop penalties along the path. Source selects the RREP with the lowest accumulated penalty (best path).

### Why not advertise the absolute composite?

- Composite metrics at different nodes aren't comparable (node A's delivery rate to node B says nothing about node C's delivery rate to node D)
- Per-hop contribution is what matters for path selection
- Keeps RREP format unchanged — metric field already exists

### Intermediate Node Route Selection

Intermediate nodes still install routes from passing RREPs using the composite metric for their local hop. They don't re-evaluate the full path — that's the source's job.

---

## 6. Hysteresis

### Minimum Switch Threshold

A new route must be better than the current route by at least `ROUTE_SWITCH_HYSTERESIS` (default: 15 metric points, ~6% of 255) to trigger a switch:

```c
#define ROUTE_SWITCH_HYSTERESIS 15

bool should_switch_route(uint8_t current_metric, uint8_t new_metric) {
    return (new_metric > current_metric + ROUTE_SWITCH_HYSTERESIS);
}
```

### Broken Route Exception

If the current route has `state == ROUTE_BROKEN` or delivery EMA drops below `DELIVERY_CRITICAL_THRESHOLD` (default: 50, ~20%), switch immediately regardless of hysteresis.

### Cooldown Timer

After a route switch, suppress further switches for `ROUTE_SWITCH_COOLDOWN` (default: 30s). Prevents oscillation in environments where two paths alternate quality.

```c
if (now - route->last_switch_time < ROUTE_SWITCH_COOLDOWN) {
    return false;  // too soon
}
```

---

## 7. Beacon Extensions

### New Fields

| Field | Type | Size | Description |
|-------|------|------|-------------|
| `airtime_remaining_pct` | uint8_t | 1B | Normal-tier airtime budget remaining (0–100%) |

Total beacon growth: **1 byte**.

### Backward Compatibility

Beacons already have a `flags` field. Use bit 0 of flags to indicate "extended beacon" format:

```c
#define BEACON_FLAG_EXTENDED_METRICS  (1 << 0)
```

Old nodes ignore the extra byte (beacon parsing uses length field). New nodes check the flag before reading `airtime_remaining_pct`. If flag is absent, assume `airtime_remaining_pct = 50` (neutral default).

### Fields NOT Added to Beacons

- **Delivery rate** — this is per-neighbor, measured locally. No need to broadcast.
- **Latency** — per-route, measured end-to-end. Not a beacon concern.
- **Composite metric** — derived, not raw data. Nodes compute their own.

---

## 8. Integration with Existing Code

### `components/routing/routing.c`

**Modified functions:**

- `route_install()` — call `compute_composite_metric()` instead of `255 - compute_link_penalty()`. Add hysteresis check before replacing existing route.
- `route_table_maintenance()` — add delivery EMA decay timer.

**New functions:**

```c
uint8_t compute_composite_metric(const neighbor_entry_t *neighbor,
                                  const route_entry_t *route);
bool    should_switch_route(const route_entry_t *current, uint8_t new_metric);
```

### `components/routing/forwarding.c`

**Modified functions:**

- `forward_data()` — after TX, increment `neighbor->delivery.tx_count`. On ACK/timeout callback, update delivery EMA.

### `components/routing/discovery.c`

**Modified functions:**

- `handle_rrep()` — use composite per-hop metric in accumulation.

### `components/airtime/airtime_budget.c`

**New functions:**

```c
uint8_t airtime_remaining_pct(void);  // for beacon construction
```

### New File: `components/routing/metrics.c`

All metric computation logic lives here:

```c
uint8_t compute_composite_metric(neighbor, route);
uint8_t compute_delivery_score(neighbor);
uint8_t compute_airtime_score(neighbor);
uint8_t compute_latency_score(route);
void    update_delivery_ema(neighbor, bool success);
void    decay_delivery_stats(neighbor, uint32_t now);
```

Keeps `routing.c` clean. Single responsibility.

---

## 9. Simulator Scenarios

### 9.1 Asymmetric Links

**Setup:** Node A↔B has good RSSI both ways. Node A→C has good RSSI but C→A is weak (asymmetric). Route A→D goes through either B or C.

**Expected:** After ~24 packets, delivery EMA for A→C path drops. Routing shifts to A→B→D even though A→C has better RSSI.

**Validates:** Delivery rate tracking catches what RSSI alone misses.

### 9.2 Congested Node

**Setup:** Node B is a hub forwarding for 5 other nodes. Node C is an alternate path with 1 other user. Both have similar link quality to destination.

**Expected:** B's `tx_queue_depth` grows, latency through B increases. Routing gradually shifts some traffic to C.

**Validates:** Latency score acts as implicit congestion signal.

### 9.3 Airtime-Depleted Path

**Setup:** Node B has used 95% of its normal airtime budget. Node C has 60% remaining. Route through B is 1 hop shorter.

**Expected:** Airtime penalty cliff kicks in for B (score quartered). Traffic routes through C despite extra hop.

**Validates:** Airtime-aware routing prevents routing through nearly-exhausted nodes.

### 9.4 Route Flapping

**Setup:** Two equal-quality paths that alternate being slightly better (±5 metric points) every few seconds.

**Expected:** Hysteresis threshold (15 points) prevents any route switches. Traffic stays on whichever path was first installed.

**Validates:** Hysteresis and cooldown timer prevent flapping.

### 9.5 Mixed Tier Traffic

**Setup:** Node B is airtime-depleted for normal tier but has critical budget. Emergency message needs routing.

**Expected:** Normal traffic avoids B. Critical traffic still routes through B if it's the best RF path.

**Validates:** Tier-aware airtime scoring.

---

## Task Breakdown

### Phase 1: Metric Infrastructure
- [ ] Create `components/routing/metrics.c` and header
- [ ] Implement `compute_composite_metric()` with integer arithmetic
- [ ] Add `neighbor_delivery_t` to neighbor table struct
- [ ] Implement `update_delivery_ema()` and `decay_delivery_stats()`
- [ ] Add delivery tracking hooks in `forwarding.c` TX path
- [ ] Unit tests for metric computation (known inputs → expected outputs)

### Phase 2: Airtime Integration
- [ ] Implement `airtime_remaining_pct()` in `airtime_budget.c`
- [ ] Add `airtime_remaining_pct` field to beacon struct
- [ ] Set `BEACON_FLAG_EXTENDED_METRICS` flag in beacon construction
- [ ] Parse extended beacon in beacon handler (backward-compatible)
- [ ] Implement `compute_airtime_score()` with penalty cliff

### Phase 3: Latency Tracking
- [ ] Add `latency_ema_ms` to route entry struct
- [ ] Hook delivery receipt handler to compute RTT and update EMA
- [ ] Implement `compute_latency_score()` with normalization
- [ ] Handle unknown latency (score = 128)

### Phase 4: Route Selection Changes
- [ ] Replace metric computation in `route_install()` with composite
- [ ] Implement `should_switch_route()` with hysteresis
- [ ] Add cooldown timer to route entry
- [ ] Update RREP forwarding in `discovery.c` to use composite per-hop metric
- [ ] Add confidence gating (fall back to link-only when sample_count < 8)

### Phase 5: Testing
- [ ] Simulator scenarios 9.1–9.5
- [ ] Soak test: 8-node mesh running 24h, verify no route flapping
- [ ] Regression: existing routing tests still pass with new metric
- [ ] Power measurement: verify metric computation doesn't measurably affect battery

### Phase 6: Tuning
- [ ] Run simulator sweep of weight combinations
- [ ] Validate hysteresis threshold (15) isn't too aggressive or too lax
- [ ] Document recommended weights for different deployment topologies (dense urban, sparse rural, linear relay chain)
