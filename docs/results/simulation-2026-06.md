# Simulation results: scale scenarios under the collision model (June 2026)

The simulator now models the shared LoRa medium: real time-on-air, collisions,
the capture effect, half-duplex radios, and listen-before-talk. This document
reports the 10/50/100/200-node scale scenarios under that model, next to the
numbers the previous (collision-free) model produced. The previous numbers
were the basis of the public "scenarios up to 200 nodes" scaling statements;
this document replaces them.

Headline: **the scale scenarios collapse under contention.** End-to-end
message delivery is 25% at 10 nodes and 0 to 10% at 50/100/200 nodes. Every
message that obtains a route is delivered; route discovery is the part that
fails, because discovery floods of ~0.5 s frames at the firmware's default
SF10 profile compete with beacon traffic that already saturates the channel
at 100+ nodes. The old 100%-delivery numbers were an artifact of a medium
that allowed unlimited simultaneous transmissions.

## The model

Implemented in `simulator/engine/sim_radio.c` (full description in
`simulator/README.md`, "Radio model"):

- **Time-on-air:** every frame occupies the medium for its real LoRa ToA,
  computed by the firmware's own `bramble_calculate_airtime_us`
  (Semtech AN1200.13). Default PHY mirrors the firmware's
  `RADIO_PROFILE_LONG_RANGE`: SF10, 125 kHz, CR 4/5, 22 dBm.
- **Collisions:** two packets overlapping in time at a receiver, both audible
  there, destroy each other unless capture applies.
- **Capture effect:** a packet at least 6 dB stronger survives an overlap if
  it started first or within the interferer's preamble. The 6 dB co-SF
  threshold follows Bor, Roedig, Voigt, Alonso, "Do LoRa Low-Power Wide-Area
  Networks Scale?" (MSWiM 2016) and SX126x co-channel rejection figures.
- **Half-duplex:** a node cannot receive while transmitting; its own
  transmissions are serialized.
- **Listen-before-talk:** mirrors `transmit_packet` in `main/mesh_task.c`:
  up to 3 CAD checks with randomized exponential backoff (50 to 300 ms base
  plus an equal random component), then transmit anyway.
- **RSSI gradient:** log-distance path loss
  `RSSI(d) = 22 dBm - 52 dB - 29 log10(d)` (d in 10 m grid units, exponent
  n = 2.9, free-space reference loss at 10 m / 915 MHz), used for capture
  comparisons. Deliverability remains disk-range gated.

Calibration: the engine reproduces the pure-ALOHA analytic collision rate.
With 40 equal-RSSI transmitters sending one 485 ms frame each at uniform
random times in a 60 s window (LBT off), measured delivery is 0.542 against
the exact finite-N expectation of 0.531 (`simulator/gosim/aloha_test.go`
shows the derivation). Unit tests cover overlap, capture timing windows,
half-duplex, TX serialization, and LBT
(`simulator/gosim/collision_test.go`).

Alongside the radio model, two simulator-bridge divergences from firmware
were fixed because they distort collision results: DATA payloads are now
encrypted with the header bound as AAD via `bramble_header_build_aad`
(matching firmware since the PR #79 AAD binding; previously NULL AAD), and
airtime-budget debits plus route-discovery retry cadence now use real ToA and
the firmware's `discovery_should_retry` schedule (5 s, then 15 s, 3 attempts,
same query_id) instead of a constant 50 ms estimate and 1.5 s re-floods with
fresh query_ids.

At SF10/125 kHz the protocol's frames are large: a 30-byte RREQ is 485 ms on
air, a 48-byte beacon 608 ms, a header-only DATA frame 322 ms. One RREQ
flood across an N-node mesh is up to N rebroadcasts (dedup-limited), so a
single route discovery in a 200-node mesh can offer ~97 seconds of airtime
to one shared channel.

## Scenario setup

`simulator/scenarios/airtime-adaptive-{10,50,100,200}.json`: grid topologies
with 120-unit (1.2 km) spacing and 150-unit (1.5 km) radio range (only
orthogonal neighbors are audible), 600 s duration, 20 scripted unicast
messages between fixed pairs, zero configured random loss. Traffic is light
on purpose; these scenarios measure protocol overhead at scale.

Reproduce:

```bash
cd simulator/gosim && go build -o bramble-gosim .
./bramble-gosim --headless --scenario ../scenarios/airtime-adaptive-200.json
./bramble-gosim --headless --no-collisions --scenario ...   # ideal-channel baseline
```

## Results

### Old model (no collisions, no ToA, constant 50 ms airtime estimates)

These are the numbers the public scaling statements rested on, reproduced at
commit `d2483b9f` (pre-collision-model main):

| Nodes | Delivery | Avg latency | Total packets |
|---|---|---|---|
| 10 | 20/20 (100%) | 24 ms | 256 |
| 50 | 20/20 (100%) | 72 ms | 1,990 |
| 100 | 20/20 (100%) | 83 ms | 3,776 |
| 200 | 20/20 (100%) | 161 ms | 7,536 |

### New model, collisions disabled (`--no-collisions`: real ToA on an ideal infinite channel)

| Nodes | Delivery | Avg latency | Offered load (sum ToA / duration) |
|---|---|---|---|
| 10 | 20/20 (100%) | 0.67 s | 0.23 erlang |
| 50 | 20/20 (100%) | 2.00 s | 1.73 erlang |
| 100 | 19/20 (95%) | 2.16 s | 3.34 erlang |
| 200 | 20/20 (100%) | 4.47 s | 6.65 erlang |

Packet counts match the old model (the protocol behaves identically; only
delivery timing and airtime accounting change). The offered-load column is
the tell: a single LoRa channel saturates at 1.0 erlang, and these runs
demand up to 6.7 simultaneous transmissions. The old delivery numbers were
physically impossible, not optimistic. Latency at realistic ToA is seconds,
not the tens of milliseconds previously reported.

### New model, full (collisions + capture + half-duplex + LBT, firmware beacon cadence)

The beacon scheduler now matches the firmware: 60 s base / 30 s churn-min /
120 s dense-max intervals with the firmware's +-5 s per-beacon jitter
(`BEACON_JITTER_MS`), and each node's first beacon gets a random phase
within one interval (real nodes boot at uncorrelated times). An earlier
revision of this document used a 15 s jitter-free scheduler whose phase-
locked beacon storms repeated identically every interval; that artifact
dominated the small-mesh numbers (10-node delivery read 25% instead of 65%).

| Nodes | Delivered / attempted | Collision rate per reception | Half-duplex drops | LBT backoffs | Offered load | Collisions |
|---|---|---|---|---|---|---|
| 10 (grid) | 13/20 (65%) | 16% | 2 | 13 | 0.18 erlang | 75 |
| 10 (cluster) | 18/20 (90%) | 11% | 16 | 64 | 0.13 erlang | 110 |
| 50 | 0/20 (0%) | 35% | 15 | 83 | 1.16 erlang | 1,540 |
| 100 | 0/20 (0%) | 36% | 86 | 240 | 2.34 erlang | 3,442 |
| 200 | 2/20 (10%) | 37% | 118 | 522 | 4.42 erlang | 6,778 |

Collision rate = collisions / (collisions + half-duplex drops + successful
receptions). Delivered messages averaged 0.4 s (10 nodes) to 5.0 s
(200 nodes) latency; the failures are route discoveries that never
completed.

The 10-node cluster row is the desk-deployment reconciliation: 10 nodes in
a single collision domain (10 m spacing, every node audible to every other,
`simulator/scenarios/cluster-10.json`) deliver 18/20 under gentle load.
That matches field experience with small real deployments and shows the
grid row's remaining failures are multi-hop discovery losses, not a claim
that small real meshes do not work.

Per-node transmit airtime over the 600 s run (real ToA):

| Nodes | min | p50 | p95 | max |
|---|---|---|---|---|
| 10 | 8.7 s | 10.0 s | 13.4 s | 13.4 s |
| 50 | 10.0 s | 13.8 s | 16.3 s | 17.2 s |
| 100 | 10.0 s | 13.8 s | 16.3 s | 16.8 s |
| 200 | 9.0 s | 13.4 s | 15.6 s | 18.4 s |

Every node spends 1.5 to 2.9% of wall time transmitting, nearly all of it
control traffic (control airtime is 86 to 100% of transmissions per run).
The EU868 duty-cycle limit is 1%; no run would be legal there (duty-cycle
enforcement landed in the firmware TX gate, workstream 2.1, after these
scenarios were defined; the simulator bridge does not yet model it).

## Where the knee is

Between 10 and 50 nodes, and the mechanism is beacon saturation plus
discovery fragility:

1. **Beacons alone exceed channel capacity at 50+ nodes.** A beacon is
   608 ms on air; at the firmware's 60 s cadence, 50 nodes offer ~0.5
   erlang of pure beacon traffic before any RREQ or user traffic, and the
   RREQ floods triggered by failed discoveries push the 50-node run to 1.16
   erlang on a channel that saturates at 1.0. At 100 and 200 nodes the
   medium is over capacity from control traffic alone (2.3 and 4.4 erlang).
2. **Route discovery requires a multi-leg round trip through that storm,
   and the firmware's own retry behavior makes it worse (LEDGER DES-2).**
   An RREQ flood must reach the destination and a unicast RREP must hop
   back along the reverse path. The firmware retries a discovery at 5 s and
   15 s with the SAME query_id, while the RREQ dedup window is 30 s, so
   every node that heard attempt 1 (including, often, the destination)
   silently drops both retries; the simulator faithfully reproduces this.
   Discovery effectively gets one flood attempt per message, and with
   per-reception collision rates of 16 to 37%, multi-hop round trips fail
   often. Fixing DES-2 (fresh query_id per retry) is the single highest-
   leverage routing change these runs point at.
3. **Synchronized rebroadcasts make floods self-destructive (DES-3).**
   Relays forward RREQs immediately on receipt with no jitter, so same-hop
   relays start transmissions at the same instant. LBT desyncs some of them
   (with LBT modeling disabled, the pre-jitter 10-node run dropped from
   5/20 to 2/20) but gives up after 3 attempts and transmits anyway.

Captures are zero in every grid run: the uniform 120-unit spacing puts every
audible interferer at exactly the same distance (equal RSSI), so the 6 dB
threshold is never met. That is a property of the synthetic grid, not the
model; capture behavior is exercised by unit tests, and irregular real-world
geometry would see some capture benefit.

## What these results do and do not say

They say: the current protocol, at the firmware's default SF10 long-range
profile, with its current discovery behavior (no rebroadcast jitter, fixed
beacon sizes, single shared channel), does not scale to dense single-channel
meshes. The "route-based forwarding scales O(path_length)" argument is about
data traffic and holds up in these runs (routed messages all delivered); it
is the control plane that saturates the channel first.

They do not say the design ceiling is 10 nodes. The same scenarios at the
medium-range profile (SF7/250 kHz) would cut every ToA by a factor of ~13
(485 ms RREQs become 36 ms), and discovery jitter, retry behavior, and
duty-cycle enforcement are open protocol-integrity items (workstreams 2.1
and 2.2). The model exists so those changes can be measured instead of
asserted.

Model limitations, stated plainly:

- No real RF propagation: range is a disk, path loss is log-distance with
  fixed parameters (n = 2.9, 52 dB at 10 m), no fading, terrain, or antenna
  effects.
- No external interference from other networks or ISM users.
- CAD is modeled as deterministic energy detection within the range disk;
  real SX126x CAD is preamble-biased and probabilistic. It is also evaluated
  once against transmissions already decided at check time, so it cannot
  sense transmissions decided later in the same instant; mildly pessimistic
  under storm conditions.
- The capture window spans the interferer's full 16.25-symbol preamble;
  Bor et al. (2016) measured capture succeeding only in roughly the last 5
  preamble symbols. No effect on these runs (capture never fired in the
  equal-RSSI grids), but irregular topologies would see slightly less
  capture benefit than modeled.
- Overlap is computed on transmit windows; propagation offsets are ignored
  (microseconds against ToA of hundreds of milliseconds).
- Single channel, single SF; no inter-SF interference modeling.
- The simulator bridge still diverges from firmware orchestration in one
  structural way: reliability/receipt logic is a parallel implementation
  (convergence is workstream 2.5). The hop-limit divergence this bullet
  used to describe (bridge flooding at hop limit 32 against the firmware's
  4) was fixed alongside the routing fixes measured in the addendum below;
  the bridge now uses the firmware's expanding-ring hop budgets.

## Raw data

All runs are deterministic (fixed topologies, scripted traffic, seeded RNG
for SNR jitter and LBT backoff draws) and reproducible with the commands
above. The old-model rows reproduce at commit `d2483b9f`.

## Addendum (2026-06-12): routing fixes measured

Workstream 2.2 changed discovery behavior (LEDGER DES-1 through DES-4).
The simulator compiles the real protocol code, so the changes flow into
these scenarios directly. Three configurations, same seeds, same scenarios:

- **A, published above:** pre-fix protocol code, but with the bridge's
  hop-limit-32 override, i.e. the numbers in the tables above. Kept for
  continuity; it overstates the old firmware's reach.
- **B, honest baseline:** pre-fix protocol code with the firmware's real
  hop budgets (RREQ 4, DATA 3). This is what shipped firmware would do.
- **C, routing fixes:** fresh query_id per retry (DES-2), 50-300 ms
  jittered RREQ rebroadcast (DES-3), expanding-ring discovery, 4 hops then
  8 on retries, with DATA/RREP/RERR budgets raised to the 8-hop route
  depth (DES-1).

| Scenario | A: published | B: honest old firmware | C: routing fixes |
|---|---|---|---|
| 10-node grid | 13/20 (65%) | 11/20 (55%) | **17/20 (85%)** |
| 10-node cluster | 18/20 (90%) | 18/20 (90%) | 18/20 (90%) |
| 50 | 0/20 (0%) | 0/20 (0%) | 1/20 (5%) |
| 100 | 0/20 (0%) | 0/20 (0%) | 1/20 (5%) |
| 200 | 2/20 (10%) | 0/20 (0%) | 0/20 (0%) |

Seed robustness on the headline row: the 10-node grid delivers 11/11/11
across seeds 42/7/1234 before the fixes and 17/17/19 after.

What the columns say, honestly:

1. **The fixes work where the channel has headroom.** On the 10-node grid
   the discovery failures the June results blamed on DES-2 are mostly
   gone: 55% to 85-95%. Average latency rose from 0.33 s to 0.65 s, the
   expected price of rebroadcast jitter and of actually delivering the
   multi-hop messages that previously failed.
2. **The fixes do not rescue saturated meshes.** At 50/100 nodes the
   channel is over capacity from control traffic before any discovery
   begins (1.2 to 2.3 erlang offered). Fresh-query retries now actually
   re-flood instead of being eaten, which roughly quadruples RREQ traffic
   on a channel that was already saturated (50-node: 290 to 1,409 RREQ
   transmissions, offered load 0.74 to 1.67 erlang). Delivery moved 0 to
   1/20. The knee is a capacity problem (beacon and discovery ToA at SF10),
   not a retry-semantics problem; profile and cadence work is outside this
   change.
3. **The 200-node scenario was never within protocol reach.** Every
   scripted pair in the 200-node grid is 11 to 17 grid-hops apart, beyond
   the protocol's 8-hop maximum route depth. The published "2/20" was an
   artifact of the bridge's hop-limit-32 override (column A); honest
   firmware delivers 0 at 200 nodes regardless of these fixes. A future
   re-scoping of that scenario should either place pairs within 8 hops or
   exist explicitly to measure out-of-reach behavior.
4. **DES-4 (route metric): measured, then deleted.** An
   accept-better-metric variant (destination re-answers a duplicate RREQ
   copy carrying a strictly better path metric; relays update reverse
   routes but do not re-flood) was implemented and measured against the
   same scenarios: delivery and latency were identical to first-arrival
   (17/18/1 on 10-grid/cluster/50, to the millisecond). The uniform grids
   cannot produce metric differentiation (all audible links have equal
   RSSI), and no other scenario exercises it either. Per the
   no-decorative-machinery rule, the unwired composite scoring module
   (`route_metric.c`) was deleted and first-arrival is now the documented
   selection rule; the penalty-based metric still arbitrates between
   discovery attempts at `route_install` (each attempt is a fresh flood
   under DES-2, so retried discoveries genuinely produce competing RREPs).

Reproduce column C with the commands in "Scenario setup" above at this
commit; column B is this commit's protocol code with the pre-fix discovery
semantics restored (same query_id on retry, no jitter, hop limit 4, DATA 3).
