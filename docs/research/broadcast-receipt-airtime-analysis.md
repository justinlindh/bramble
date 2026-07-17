# Broadcast Delivery Receipt Airtime Analysis (Bramble)

Date: 2026-03-02

## Executive Summary

Broadcast delivery receipts in the current form do **not scale** past small meshes. With 3 receipt attempts per recipient and multi-hop forwarding, receipt traffic alone can exceed practical mesh airtime budgets by 10–100x in 50–200 node networks.

Two additional findings make this more urgent:

1. In firmware, broadcast delivery receipts are currently **not budget-gated** by `airtime_budget_can_transmit()` and are not debited into airtime token buckets (only data/beacons are debited in current code paths).
2. Current receipt sending runs `vTaskDelay()` in mesh task context during slotting/retries, which can block packet processing and worsen congestion at scale.

Recommendation: make broadcast receipts **tier- and mode-dependent**, default OFF for normal broadcast chat/telemetry, and use sampled/neighbor-scoped acknowledgments for large networks.

---

## 1) Current implementation facts (from repo)

### Receipt behavior
- File: `main/broadcast_delivery_receipt.c`
  - `BROADCAST_RECEIPT_RETRY_COUNT = 3`
  - Slotting: 32 buckets, 200 ms base, 200 ms spacing
- File: `main/mesh_task.c`
  - `send_broadcast_delivery_receipt()` adds slot delay + jitter, then sends 3 attempts with exponential delays.
  - `forward_delivery_receipt()` relays receipts hop-by-hop back toward source.

### Airtime budget implementation
- Budgets (`components/airtime/include/airtime_budget.h`):
  - critical 36 s/hour, normal 18 s/hour, broadcast 18 s/hour
- `airtime_budget_can_transmit()` exists, but is not used in TX hot path.
- In `mesh_task.c`, airtime debit currently occurs in:
  - `send_beacon()`
  - `send_data_packet()`
- Receipt TX/forward paths do not debit budget and are not blocked by budget checks.

### LBT/CAD behavior
- `transmit_packet()` does CAD/LBT retries (up to 3 checks) before TX.
- CAD adds radio-time overhead and contention delay, but does not reduce RF channel occupancy from receipt storms.

---

## 2) Airtime cost math at scale

## Model

Let:
- `N` = total nodes in mesh
- recipients per broadcast = `N-1`
- retries per recipient receipt = `R = 3`
- per-receipt TX airtime = `Ttx` (150–200 ms from project estimate; note: Semtech-style formula can be higher depending CR/preamble)
- average hop count from recipient back to source for receipt path = `Havg`

Then receipt airtime consumed **network-wide per one broadcast**:

`A_receipt_total = (N-1) * R * Ttx * Havg`

Single-hop best-case (`Havg=1`) lower bound:

`A_singlehop = (N-1) * 3 * Ttx`

### Table A — single-hop lower bound (best case)

| N | Receipt airtime per broadcast (150–200 ms/TX) |
|---:|---:|
| 5 | 1.8–2.4 s |
| 10 | 4.1–5.4 s |
| 20 | 8.6–11.4 s |
| 50 | 22.1–29.4 s |
| 100 | 44.6–59.4 s |
| 200 | 89.6–119.4 s |

Already at N=50+, even single-hop lower bound is severe.

### Table B — multi-hop realistic estimate

Using rough mesh-diameter growth assumptions for `Havg`:
- N=5: 1.5
- N=10: 2
- N=20: 2.5
- N=50: 3.5
- N=100: 4.5
- N=200: 6

| N | Havg | Receipt airtime per broadcast (150–200 ms/TX) |
|---:|---:|---:|
| 5 | 1.5 | 2.7–3.6 s |
| 10 | 2.0 | 8.1–10.8 s |
| 20 | 2.5 | 21.4–28.5 s |
| 50 | 3.5 | 77.2–102.9 s |
| 100 | 4.5 | 200.5–267.3 s |
| 200 | 6.0 | 537.3–716.4 s |

Interpretation: in 100+ node multi-hop meshes, one broadcast can cost **minutes** of cumulative mesh airtime in receipt traffic.

### CAD overhead note
CAD/LBT adds additional per-attempt listen time and random backoff delays. This does not directly add RF transmit airtime, but increases latency and local radio occupancy and can create further queue pressure during receipt waves.

---

## 3) Comparison: why other protocols avoid per-node broadcast ACK/receipts

## Meshtastic
Meshtastic docs explicitly note that broadcast ACK behavior is special-cased/suppressed because ACKing broadcasts would flood channels. Typical strategy is inference from rebroadcasts rather than per-node confirmation.

## LoRaWAN
LoRaWAN downlinks are constrained and individually addressed. It does not do “every receiver ACKs a broadcast” at scale because this is structurally incompatible with LPWAN duty/airtime economics.

## Why this pattern is common
Per-recipient ACK/receipt for broadcast is effectively O(N) at minimum and O(N·H) in multi-hop. In low-throughput LoRa meshes, this rapidly dominates capacity and destabilizes delivery for all traffic classes.

---

## 4) Recommendations for Bramble

## A. Make broadcast receipts optional by tier (strong recommendation)

Default matrix:
- **Broadcast tier:** receipts OFF
- **Normal tier:** receipts OFF by default (optional sampled mode)
- **Critical tier:** receipts ON (bounded)

Rationale: aligns reliability cost with message criticality.

## B. Replace “all recipients ack” with scalable modes

Add policy modes:
1. `OFF` — no broadcast receipts
2. `NEIGHBORS_ONLY` — only direct neighbors of sender reply
3. `SAMPLED_P` — each receiver replies with probability `p`
4. `TARGET_K` — sender requests approx K responses (receiver uses hash threshold)
5. `CRITICAL_FULL` — current full behavior only for critical emergency messages

Good default for large meshes: `TARGET_K=3..8` or `SAMPLED_P=min(1, K/(N_est-1))`.

## C. Add size-aware adaptation

Based on estimated neighbor/network size (`N_est` from probe/beacon stats):
- small (`N_est <= 10`): allow richer receipts
- medium (`10 < N_est <= 40`): sampled mode
- large (`N_est > 40`): neighbors-only or off except critical

## D. Budget enforcement fix (required)

Before sending/forwarding receipt:
- compute airtime estimate for packet size and PHY
- call `airtime_budget_can_transmit()` for selected tier/class
- only transmit when allowed; otherwise drop/defer
- debit on success

Without this, budget settings are informational rather than protective for receipt storms.

## E. Move receipt timing/retry out of mesh task blocking path

Current `vTaskDelay()` inside mesh task for receipt staggering/retries can block packet processing. Use timer/callback or dedicated lightweight sender task/queue.

---

## 5) Dynamic airtime budget policy proposal

Current fixed budgets:
- critical 36 s/h
- normal 18 s/h
- broadcast 18 s/h

Proposed configurable policy:

```c
airtime.dynamic_enabled = true
airtime.base_budget_ms_per_hour = 72000   // total node tx budget target
airtime.min_budget_ms_per_hour  = 18000
airtime.max_budget_ms_per_hour  = 180000
airtime.neighbor_scale_alpha    = 0.5     // compression with density
airtime.broadcast_share_small   = 0.25
airtime.broadcast_share_large   = 0.05
airtime.critical_floor_share    = 0.40
```

Behavior:
- as density grows, reduce broadcast share and non-critical retry allowances
- preserve critical floor share
- apply same policy to receipt traffic class

---

## 6) Simulator coverage and needed updates (documented only)

Findings:
- Simulator has delivery-receipt support in `simulator/gosim/bridge.c` including relay path.
- But simulation currently sends one modeled receipt path event; it does **not** fully model firmware receipt behavior (slot buckets + jitter + 3 retries + mesh-task blocking + budget gating interactions).

Recommended simulator updates:
1. Add receipt policy model (OFF/NEIGHBORS/SAMPLED/TARGET_K/FULL).
2. Model 3-attempt receipt retransmission with slot/backoff timing parity to firmware.
3. Add per-tier budget gate/debit simulation for receipts.
4. Add metrics:
   - receipt_tx_count
   - receipt_airtime_ms
   - receipt_collision_rate
   - receipt_success_ratio
   - % airtime spent on control vs user data
5. Add scenarios for N=10/50/100/200 with varying diameter/churn and rapid-fire broadcasts.

---

## 7) Regulatory/FCC context (915 MHz US)

Relevant reference: 47 CFR §15.247.

Key points:
- US 902–928 MHz under Part 15.247 does not impose the same simple duty-cycle rule style as EU 868 MHz.
- Compliance depends on modulation category constraints (e.g., hopping occupancy rules for FHSS and bandwidth/power rules for digital modulation).
- Even when legally permissible, excessive occupancy is still operationally harmful in shared ISM use and can self-deny mesh performance.

Engineering takeaway: design to strict self-imposed airtime discipline regardless of legal headroom.

---

## 8) Direct answers to the requested questions

1. **Actual airtime cost at N=5..200?**
   - Single-hop lower bound and multi-hop realistic estimates are provided in Tables A/B.
   - At 100+ nodes, full per-recipient 3x receipt strategy is not operationally sustainable.

2. **Do delivery receipts scale?**
   - Not in full per-node form for broadcast. Industry mesh/LPWAN practice avoids this for exactly the same airtime economics.

3. **Should receipts be optional/configurable by tier?**
   - Yes. Strongly recommended. Critical-only full receipts; lower tiers sampled or off.

4. **Should behavior change with network size?**
   - Yes. Use automatic mode switching based on `N_est` and congestion state.

5. **Should budgets be dynamic?**
   - Yes, with critical floor and shrinking broadcast/control shares in dense conditions.

6. **FCC/regulatory implications?**
   - US rules are more permissive than EU duty-cycle regime, but still bounded by Part 15.247 conditions. Practical mesh capacity, not legal max alone, should drive design.

---

## 9) Priority action list

1. Implement receipt policy switch (default OFF for broadcast tier).
2. Enforce airtime budget for receipt send/forward paths.
3. Unblock mesh task from receipt delay/retry sleeps.
4. Add simulator parity for receipt retry/slot policy and policy modes.
5. Validate with 10/50/100/200 node scenarios and publish receipt airtime fraction.
