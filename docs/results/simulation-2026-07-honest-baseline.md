# Honest Scale Baseline (July 2026)

**Phase 0 gate artifact** for the scalable-mesh recovery program. Supersedes the scale numbers in `simulation-2026-06.md` for planning purposes; the June collapse *diagnosis* survives, but it was measured on a simulator that did not run the firmware's own control machinery, and its headline delivery metric had a wrong denominator (both fixed here).

## What changed in the simulator (Phase 0, Tasks 1-6)

The June numbers were produced by a sim with parallel implementations and missing gates. As of this branch, gosim:

1. **Budget-gates every transmission** through the real `airtime_budget_can_transmit`/`_debit` (beacons, RREQ/RREP/RERR, receipts, data), with the firmware's tier mapping. Previously only unicast DATA was gated. Also fixed: the sim never set the budget's peer count, so profile scaling never engaged at all.
2. **Runs the real RREQ rate limiters** (`rreq_rate_allow` at origination, `rreq_fwd_allow` at forward), compiled from `components/security/security.c`, wired at the same decision points as `mesh_task.c`.
3. **Runs the real beacon policy** (`main/beacon_policy_calc.c`); the sim's hand-copied controller is deleted. Default scenarios use the firmware's shipped default: fixed 60 s beacons, adaptivity off.
4. **Measures airtime honestly**: per-type ToA accumulators charged once at the single TX chokepoint; `control_airtime_pct` is now genuinely ToA-weighted (the old packet-count ratio is kept as `control_packet_pct`).
5. **Models regulatory duty cycle** on request (off for this baseline, matching the June conditions).
6. **Reproduces the legacy scenarios byte-for-byte** from a parameterized generator, so this baseline is a valid before/after against June.

**Metric correction:** the old headline `delivery_rate` divides delivered messages by *total packets of every type on the air* (19 delivered / 203 packets = 9.4% at 10 nodes), which is not a delivery figure. The honest end-to-end number is `message_delivery_rate` = delivered / (delivered + dropped + undelivered), added in this branch. All figures below use it. (In all 12 runs the terminal states sum to exactly the 20 scripted messages.)

## Baseline results (firmware defaults: SF10/125 kHz, fixed 60 s beacons, duty off)

Grid topology, 120-unit spacing (each node hears ~4 neighbors), 20 scripted messages over 600 s, 3 seeds per node count.

| Nodes | Message delivery (mean, spread) | Offered load (erlangs) | Control share of ToA | RREQ ToA (s/600s) | Beacon ToA (s/600s) |
| --- | --- | --- | --- | --- | --- |
| 10 | **95%** (95-95) | 0.18 | 82% | 16 | 65 |
| 50 | **12%** (5-15) | 1.7 | 99.5-99.8% | 679-691 | 321 |
| 100 | **10%** (10-10) | 2.4 | 99.5-99.7% | 721-778 | 644 |
| 200 | **0%** (0-0) | 3.5 | 100% | 817-821 | 1287 |

Seeds verified to drive the RNG (distinct event timelines); at n=10 all three seeds converge to identical aggregates (small, dense, well-connected topology), so n=10 effectively contributes one observation, not three. Runs cost 0.13-0.24 s wall-clock each; parameter sweeps are cheap.

## Findings

1. **The collapse is confirmed, with the firmware's real machinery running.** A single SF10 channel saturates (>1.0 erlang offered) at 50 nodes and beyond; delivery collapses from 95% at 10 nodes to ~12% at 50 and 0% at 200. The honest sim does not soften the June conclusion; it hardens it.
2. **The firmware's own throttles never fire at this profile.** Across all 12 runs: zero budget denials in any lane, zero RREQ origination or forward rate-limit denials. The budget profiles are calibrated to LOCAL neighbor density (~4 in this grid, which selects the most generous 400% "micro mesh" profile) while the failure is GLOBAL channel saturation. The shipped admission control does not and cannot see this collapse; that is the core calibration gap for Phase 2.
3. **Discovery, not beacons, is the largest airtime consumer at 50-100 nodes once traffic flows.** At n=50, RREQ flooding burns ~680 s of ToA against ~321 s of beacons over a 600 s run (RREQ alone is ~1.13 erlangs; ~1,400 RREQ transmissions for 20 message attempts, roughly 70 per attempt, because failed discoveries retrigger full expanding-ring refloods into an already saturated channel). Beacons are the load FLOOR (0.54 erlang at 50 nodes, saturating on their own before any traffic); the discovery storm is the AMPLIFIER. Both levers are required: beacon cadence work alone will not rescue 50 nodes under load, and discovery-cost work alone starts from an already-saturated floor.
4. **At 200 nodes beacons dominate outright** (1,287 s vs 821 s RREQ) and nothing else matters: the medium is 3.5x oversubscribed by control traffic and no data is ever sent. (The 200-node scenario also spans 11-17 grid hops, beyond the 8-hop ceiling; it remains out of scope for the program bar.)
5. **Delta vs June:** direction identical, magnitudes slightly kinder at 50/100 (12%/10% honest vs 0-5% reported in June, measured under a corrected metric and with the real gates present). The June doc's qualitative diagnosis (control-plane saturation) stands; its delivery percentages and its implied "the sim models the firmware" claim do not.

## What this baseline commits us to (per the roadmap)

- The **Phase 1** delivery-core fixes (data-path reverse-route learning) directly attack finding 3's amplifier: routes learned from forwarded traffic mean fewer full refloods per message.
- The **Phase 2** lever order stands, with finding 3 sharpening it: beacon cadence (the floor) and discovery amortization (the amplifier) are co-primary; the channel-utilization-driven admission redesign (finding 2) is the calibration fix that makes the budget machinery see global saturation.
- Feasibility math unchanged: at Meshtastic-comparable beacon cadence (15 min), the 50-node beacon floor drops from 0.54 to ~0.036 erlang. The channel budget for the program bar exists; this baseline documents exactly what is spending it today.

## Reproduction

```bash
cd simulator/gosim && go build -o bramble-gosim .
python3 ../scenarios/generate.py --legacy N --seed S --out /tmp/nN-sS.json   # N in {10,50,100,200}, S in {1,2,3}
./bramble-gosim --headless --scenario /tmp/nN-sS.json   # read final_metrics.message_delivery_rate
```

## Phase 1 addendum (delivery-core): de-masking, then fixing, the reverse-route gap

The Phase 1 delivery-core plan the internal design plan found that the numbers above were themselves optimistic in a way this baseline could not see: gosim installed a route from every beacon it processed (`route_install(&rx->routes, beacon.src_addr, beacon.src_addr, 1, ...)` in `bridge.c`), something firmware's own `handle_beacon` never does. That accidentally supplied gosim's nodes with the reverse-hop routes real firmware lacked, so the sim could not reveal the confirmation-return bug the plan set out to fix: relays only ever learned routes TOWARD discovery targets, never back toward a DATA message's originator, so a destination's ACK/delivery receipt died at the first relay and the sender saw a genuinely-delivered message reported as failed. These numbers correct that masking and then measure the fix, using the exact same legacy 10/50-node generator, topology, and seeds as the table above, so they are a direct before/after against it, not a new methodology.

**10-node `message_delivery_rate` (all 3 seeds converge identically, as above):**

| Stage | Rate | What changed |
| --- | --- | --- |
| This baseline (masked) | 95% | gosim's beacon-derived routes hid the bug |
| De-masked (Task 1: beacon route-install removed) | **40%** | The TRUE parity number once gosim can no longer cheat: the confirmation-return bug, now visible, tanks sender-observed delivery even though destinations were still receiving the message |
| Wire v4 reverse-route learning (Task 4/4-fix) | **50%** | DATA-driven breadcrumbs (`docs/SECURITY-MODEL.md`, §4.27 of the protocol spec) give a returning ACK a route home at every relay; +10 points over the de-masked baseline, the direction and magnitude the plan required before calling the fix real |

**50-node `message_delivery_rate`:** stayed flat within seed noise, ~12% before de-masking, 5-15% (mean ~10%) after both de-masking and the wire v4 fix. At this node count the mesh is already control-plane-saturated (finding 1 above); a handful of reverse-route breadcrumbs cutting occasional rediscovery does not move a number this dominated by RREQ-flood airtime. This is an honest flat result, not a regression: Task 4's own gosim scenario (a >=3-hop line topology, not this saturated grid) directly demonstrates the mechanism works when the mesh has headroom to show it.

**Both numbers measure something more honest than the July baseline's headline did.** The July 95%/12% number above was itself only ever a destination-delivery figure; it could not distinguish "the destination received the message" from "the sender was correctly told so," because gosim's masked routing made every reverse path free. The 40%/50%/12% figures above are the first in this document's history to measure destination delivery AND (for unicast) genuine sender-visible confirmation together, which is what "delivery" has to mean for the DELIVERED/FAILED distinction this program promises to be honest about.

**Channel flood at 50 nodes amplifies the existing saturation rather than being bounded by it (Task 5).** Adding 10 broadcast/channel messages (one per 60s) on top of the unchanged 20-message unicast workload, at the same 50-node topology and seeds: unicast delivery, already only 1-3 messages out of 20 scripted attempts (5-15%) with no flood traffic, drops to **0 delivered in all 3 seeds** once flood traffic is added. DATA-type airtime rises 20-40x (an extra ~20% of the entire 600-second budget window spent on just 10 broadcasts). The flood's own airtime-aware relay gate (`channel_flood_decide`, real budget-gated) never once denies a relay in any of these runs: not because it is broken, but because the airtime budget profile selected at this density is calibrated to LOCAL neighbor density, not GLOBAL channel occupancy, so a dense 50-node grid keeps picking a generous local profile no matter how saturated the shared channel already is. This is Phase 0 finding #2, previously diagnosed as a calibration gap and scheduled for Phase 2's admission-control redesign; this addendum's contribution is showing that gap now actively amplifies harm under multi-hop channel traffic, not merely persisting inertly. At 10 nodes, where there is real headroom, the same flood adds real cost (DATA airtime rises ~4-5x) without visibly hurting the 10 scripted unicast messages' delivery (10/10 in every seed, with and without flood).

**Caveat on the flood numbers.** gosim's aggregate `delivered`/`dropped`/`message_delivery_rate` metrics cannot credit a broadcast as delivered (the bookkeeping structure they read from is inherently unicast-shaped); every one of the 10 added broadcasts in these runs lands in `undelivered` by construction, even on the 10-node runs where a dedicated gosim scenario (`TestPhase1ChannelFloodReachesFarNode`, a 5-node line) separately proves the flood reaches a node 4 hops from the sender. Read the unicast delivery collapse above from the `delivered`/`dropped` columns of the SAME scripted unicast messages, isolated from the flood traffic layered on top, not from the aggregate rate.

Methodology: every number above comes from the gosim scenario runner (`simulator/gosim/`, scenarios in `simulator/gosim/scenarios/`); re-run any scenario JSON with the harness there to reproduce.
