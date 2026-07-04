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
|---|---|---|---|---|---|
| 10  | **95%** (95-95) | 0.18 | 82% | 16 | 65 |
| 50  | **12%** (5-15)  | 1.7  | 99.5-99.8% | 679-691 | 321 |
| 100 | **10%** (10-10) | 2.4  | 99.5-99.7% | 721-778 | 644 |
| 200 | **0%** (0-0)    | 3.5  | 100%       | 817-821 | 1287 |

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

```
cd simulator/gosim && go build -o bramble-gosim .
python3 ../scenarios/generate.py --legacy N --seed S --out /tmp/nN-sS.json   # N in {10,50,100,200}, S in {1,2,3}
./bramble-gosim --headless --scenario /tmp/nN-sS.json   # read final_metrics.message_delivery_rate
```
