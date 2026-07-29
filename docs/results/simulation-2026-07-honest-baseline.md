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

## Baseline results as published in July (SF10/125 kHz, fixed 60 s beacons, duty off)

**Superseded, and the PHY label was wrong.** These runs were priced at SF10/125 kHz, which is not the firmware default: `mesh_init_radio_config` overwrites `RADIO_PROFILE_LONG_RANGE`'s SF with the frequency plan's, and every shipped plan defaults to SF9/125 kHz, so a stock node's boot log reads `SF9 BW125000`. Every frame in the table below was therefore charged about 1.9x its true time-on-air, which inflates the offered-load, control-share and airtime columns directly. The re-run at the corrected PHY is the section below, dated 2026-07-24; the table here is kept as the historical record of what was published, not as a current number.

Grid topology, 120-unit spacing (each node hears ~4 neighbors), 20 scripted messages over 600 s, 3 seeds per node count.

| Nodes | Message delivery (mean, spread) | Offered load (erlangs) | Control share of ToA | RREQ ToA (s/600s) | Beacon ToA (s/600s) |
| --- | --- | --- | --- | --- | --- |
| 10 | **95%** (95-95) | 0.18 | 82% | 16 | 65 |
| 50 | **12%** (5-15) | 1.7 | 99.5-99.8% | 679-691 | 321 |
| 100 | **10%** (10-10) | 2.4 | 99.5-99.7% | 721-778 | 644 |
| 200 | **0%** (0-0) | 3.5 | 100% | 817-821 | 1287 |

Seeds verified to drive the RNG (distinct event timelines); at n=10 all three seeds converge to identical aggregates (small, dense, well-connected topology), so n=10 effectively contributes one observation, not three. Runs cost 0.13-0.24 s wall-clock each; parameter sweeps are cheap.

## Re-run at the corrected PHY (2026-07-24)

The simulator now prices airtime at the frequency plan's SF9/125 kHz, the PHY a stock node actually transmits at, instead of the radio-profile table's SF10. Same generator, same topologies, same three seeds: `generate.py --legacy N --seed S` for N in {10,50,100,200} and S in {1,2,3}, 12 runs per column. Both columns were measured at the same commit, so the SF10 column isolates the PHY change rather than mixing it with everything else that landed since July. The SF10 column also does not reproduce the July table above (30% against 95% at n=10), because the protocol code moved underneath it: the largest single step is documented in the Phase 1 addendum below, where removing gosim's beacon-derived route-install de-masked the confirmation-return bug and took n=10 from 95% to 40%.

| Nodes | Message delivery | Confirmed delivery | Offered load (erlangs) | Control share of ToA | RREQ ToA (s/600s) | Beacon ToA (s/600s) | Mean latency |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 10, SF10 (wrong PHY) | 30% | 25-30% | 0.42 | 84.7% | 118 | 65 | 1.22 s |
| 10, **SF9 (corrected)** | **30%** | **30%** | **0.21** | **91.5%** | **61** | **37** | **0.63 s** |
| 50, SF10 | 5-10% | 0-5% | 1.70 | 97.6-99.2% | 674-679 | 323 | 2.0-4.9 s |
| 50, **SF9** | **10%** | **10%** | **0.91** | **97.3-97.8%** | **338-349** | **182** | **1.04-1.21 s** |
| 100, SF10 | 0-5% | 0-5% | 2.39 | 96.8-98.1% | 692-719 | 645 | 2.0 s |
| 100, **SF9** | **0-5%** | **0-5%** | **1.28** | **94.6-95.5%** | **332-345** | **363** | **1.04 s** |
| 200, SF10 | 0% | 0% | 3.55 | 100% | 830-845 | 1289 | n/a |
| 200, **SF9** | **0%** | **0%** | **1.94** | **100%** | **433-440** | **726** | **n/a** |

What the correction does and does not change:

1. **Airtime, offered load and latency all fall by about 1.85x**, the ToA ratio between SF10 and SF9 for these frame sizes (a 60-byte frame is 731 ms at SF10 and 386 ms at SF9). Every airtime-derived figure this document ever published was inflated by roughly that factor. The direction is the kinder one: the modeled channel was more congested than a real one.
2. **The collapse survives the correction, and the knee stays between 10 and 50 nodes.** Delivery is unchanged at 10, 100 and 200 nodes and moves from 5-10% to a steady 10% at 50. Halving every frame's cost does not fix a control plane whose failure mode is flood dynamics rather than a marginal capacity shortfall.
3. **Finding 1's ">1.0 erlang at 50 nodes" no longer holds and must be restated.** At the corrected PHY, 50 nodes offer 0.91 erlang: at the edge of a single channel's capacity rather than over it. Saturation past 1.0 erlang now begins at 100 nodes (1.28) and is severe at 200 (1.94). Delivery at 50 nodes is still 10%, so being nominally under capacity does not rescue it.
4. **Control share of ToA gets WORSE at 10 nodes, 84.7% to 91.5%, and that is not a regression.** Absolute control airtime halved (RREQ 118 s to 61 s, beacons 65 s to 37 s). The share rose because the data plane's airtime fell further still (DATA 17.0 s to 3.5 s, receipts 21.4 s to 7.0 s at n=10 seed 1): a less congested channel needs far fewer ACK retransmissions to deliver the same messages. Any statement of the form "control traffic is N% of airtime" is a ratio of two numbers that both moved.
5. **Findings 2, 3 and 4 stand.** Zero budget denials and zero RREQ rate-limit denials in all 24 runs, both PHYs: the shipped admission control still cannot see global channel saturation. RREQ still dominates beacons at 50 and 100 nodes (343 s against 182 s at n=50) and beacons still dominate outright at 200 (726 s against 436 s).
6. **The 15-minute-cadence feasibility math in "What this baseline commits us to" is now conservative.** The 50-node beacon floor at 60 s cadence measures 0.30 erlang, not 0.54, so the headroom that argument depends on is larger than stated, not smaller.

## Findings

The five findings below were written against the July table, i.e. at the SF10 model. Read them with the delta list in the re-run section above: item 3 there retracts finding 1's erlang figure, item 4 restates finding 4's airtime shares, and the rest survive.

1. **The collapse is confirmed, with the firmware's real machinery running.** A single SF10 channel saturates (>1.0 erlang offered) at 50 nodes and beyond; delivery collapses from 95% at 10 nodes to ~12% at 50 and 0% at 200. The honest sim does not soften the June conclusion; it hardens it. (*Erlang figure retracted 2026-07-24: it was measured at SF10 rather than the plan's SF9. At the corrected PHY, 50 nodes offer 0.91 erlang and saturation past 1.0 begins at 100. The collapse itself stands.*)
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

Methodology: every number above comes from the gosim scenario runner (`simulator/gosim/`), driven with the scenario JSON that `simulator/scenarios/generate.py --legacy` emits for the scale runs, or the committed files in `simulator/scenarios/` for the named ones; re-run any scenario JSON with the commands under "Reproduction" to reproduce.

## Receipt reliability: the LBT defer fix, before and after (2026-07-28)

### Bench finding

Bench testing of a uniform-1.6.0 fleet measured broadcast delivery receipt return at roughly 75-80% during receipt storms, a receipt storm being the burst of receipts that a broadcast triggers when several hearers all originate a reply back at the sender within seconds of each other. This was root-caused to `components/radio/tx_gate.c`'s Listen-Before-Talk loop: after three busy CAD checks on any TX kind, tx_gate transmitted anyway ("blind-fire," to avoid starving lanes that cannot afford to wait indefinitely), and a receipt storm keeps the channel busy long enough that most blind-fired receipt attempts collided at the origin and were lost. The bench evidence for this figure lives in the repository's history and in the project's internal tracking; specific bench hardware, addresses and RSSI figures are kept out of this public document by house rule. The simulation below reproduces the mechanism directly rather than restating the bench number.

### What the simulator gained

Before this branch, gosim had no way to measure broadcast receipt return at all. Four pieces landed to make an honest before/after possible:

- A `receipt_return_rate` metric (`broadcast_receipts_registered` / `broadcast_receipts_expected`, counted from the same firmware bookkeeping that records real delivery events at the origin, not a sim-only shortcut).
- A ten-node all-in-range storm scenario (`simulator/gosim/receipt_storm_test.go`) sized so one broadcast owes exactly nine receipts, with a lead-in long enough for every node's neighbor table, and therefore its anti-collision slot spacing, to be populated before the broadcast fires.
- Verification that `tx_gate`'s `ops.channel_busy` reflects the ether's real carrier state in sim rather than reading "never busy," which would have made the blind-fire defect impossible to reproduce at all.
- The firmware fix itself: `components/radio/tx_gate.c` now answers `TX_GATE_ERR_CHANNEL_BUSY` for `TX_KIND_RECEIPT` after LBT exhaustion instead of transmitting, and `main/mesh_reliability.c`'s `mesh_process_receipt_tx_event` reschedules the same attempt with fresh 250-999ms jitter, capped at 8 defers (after which it counts as a consumed attempt, so a permanently jammed channel still terminates the same way it always did). Every other TX kind keeps the original anti-starvation blind-fire; deferring is receipt-only by design, because a receipt's app-layer retry budget (three attempts, 12s) can afford to wait where data, ACK and routing traffic cannot.

The storm test runs both the pre-fix and post-fix world from the same code, the same firmware receipt policy (slot delay, jitter, three attempts, scaled backoff), and the same ten seeds, switched by a single scenario field (`receipt_tx_kind`): `receipt_forward` is the undeferred kind every receipt used before the fix, `receipt` is the deferred one after it.

### A/B result: ten-node storm, ten seeds (simulation)

`go test -run TestReceiptStormLBTDeferBeatsBlindFire -v` in `simulator/gosim`, one broadcast per seed, nine receipts owed:

| seed | blind-fire rate | defer rate |
| --- | --- | --- |
| 1 | 0.8889 | 1.0000 |
| 2 | 1.0000 | 1.0000 |
| 3 | 0.8889 | 1.0000 |
| 4 | 0.6667 | 0.8889 |
| 5 | 0.8889 | 1.0000 |
| 6 | 0.5556 | 1.0000 |
| 7 | 0.8889 | 1.0000 |
| 8 | 0.8889 | 1.0000 |
| 9 | 0.8889 | 1.0000 |
| 10 | 0.8889 | 1.0000 |
| **mean** | **0.8444** | **0.9889** |

The blind-fire arm reproduces the defect (84.4% mean), milder than the bench's 75-80% because this scenario runs the frequency plan's default SF9 rather than the bench's SF10, so a receipt occupies less air here and contends less. The defer arm clears the campaign's 95% exit gate at a 98.9% mean and beats the blind-fire arm on every seed except the one (seed 2) where both arms already returned 100%.

Seed 6 is the sharpest single-seed illustration: 179 collisions in the blind-fire arm against 116 in the defer arm, receipt_return_rate 0.5556 against 1.0000. Deferring does not eliminate contention outright (the defer arm still has a nonzero collision count, from other traffic sharing the channel); it removes the specific failure mode where a receipt collides because it fired anyway into a channel LBT had just reported busy.

### Zero extra airtime (simulation)

Receipt-tier and total time-on-air are identical between arms on every one of the ten seeds:

| | receipt-tier ToA | total ToA |
| --- | --- | --- |
| blind-fire | 7658 ms | 21622 ms |
| defer | 7658 ms | 21622 ms |

No attempt in the defer arm ever hit the eight-defer cap in this scenario, so exactly the same 27 receipt transmissions (nine hearers times three attempts) flew in both arms; the fix changes when they fly, not how many. The defer arm buys 14.4 points of receipt return for zero extra airtime: every collision it avoids is airtime that would otherwise have been spent and wasted.

### Regression sweep: legacy scale scenarios (simulation)

The legacy scale generator's scenarios (`generate.py --legacy N --seed S`) carry no broadcast traffic, only scripted unicast messages, so they cannot exercise `TX_KIND_RECEIPT`'s defer path at all: a clean regression check for everything else the branch touches. This branch's build (`fix/receipt-lbt-defer` at `988f36c4`) was compared against `main` at the branch's merge base (`22c8f7e3`), same generator, same topologies, same seeds:

| Scenario | message_delivery_rate before | after | airtime by type |
| --- | --- | --- | --- |
| n=10, seed 1 | 30% | 30% | identical |
| n=10, seed 2 | 30% | 30% | identical |
| n=10, seed 3 | 30% | 30% | identical |
| n=50, seed 1 | 10% | 10% | identical |
| n=50, seed 2 | 10% | 10% | identical |
| n=50, seed 3 | 10% | 10% | identical |

Every field in `final_metrics` came back byte-identical between before and after on all six runs, including every per-type airtime figure (`ack`, `beacon`, `data`, `receipt`, `rerr`, `rrep`, `rreq`), with one exception: the three fields Task 1 added to the metrics struct (`broadcast_receipts_expected`, `broadcast_receipts_registered`, `receipt_return_rate`), which read 0/0/1 in both before and after because these scenarios never broadcast. This is the expected result: no movement in `message_delivery_rate`, and no movement in airtime by type, because these particular scenarios contain no broadcast traffic for the fix to touch. A regression here would have meant the fix reached beyond `TX_KIND_RECEIPT`, which it does not.

### Bench verification: pending

The A/B and the regression sweep above are simulation only, run against the sim's model of the firmware's real `tx_gate` and receipt-queue logic; neither is a bench measurement. Bench verification of the fix, flashing the release build to the fleet and re-running receipt-trace rounds against the same kind of storm the bench originally measured at 75-80%, has not happened as of this section. That step requires physical hardware this task does not have access to and is not claimed here.

### Reproduction

```bash
cd simulator/gosim && go test -run TestReceiptStormLBTDeferBeatsBlindFire -v   # A/B storm, both arms, 10 seeds
python3 ../scenarios/generate.py --legacy N --seed S --out /tmp/nN-sS.json    # N in {10,50}, S in {1,2,3}
./bramble-gosim --headless --scenario /tmp/nN-sS.json                         # read final_metrics
```
