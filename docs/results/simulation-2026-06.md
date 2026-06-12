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

### New model, full (collisions + capture + half-duplex + LBT)

| Nodes | Delivered / attempted | Got a route | Collision rate per reception | Half-duplex drops | LBT backoffs | Offered load | Collisions |
|---|---|---|---|---|---|---|---|
| 10 | 5/20 (25%) | 5 | 57% | 71 | 248 | 0.28 erlang | 452 |
| 50 | 0/20 (0%) | 0 | 68% | 339 | 1,327 | 1.35 erlang | 3,394 |
| 100 | 2/20 (10%) | 2 | 74% | 605 | 2,487 | 2.58 erlang | 7,421 |
| 200 | 1/20 (5%) | 1 | 66% | 2,222 | 5,366 | 4.81 erlang | 12,575 |

Collision rate = collisions / (collisions + half-duplex drops + successful
receptions). Every message that obtained a route was delivered, at 0.6 s
(10 nodes) to 5.0 s (200 nodes) average latency; the failures are all route
discoveries that never completed.

Per-node transmit airtime over the 600 s run (real ToA):

| Nodes | min | p50 | p95 | max |
|---|---|---|---|---|
| 10 | 14.3 s | 17.0 s | 18.3 s | 19.5 s |
| 50 | 12.5 s | 15.9 s | 18.8 s | 19.3 s |
| 100 | 12.5 s | 14.9 s | 19.8 s | 19.8 s |
| 200 | 11.1 s | 14.0 s | 18.8 s | 19.8 s |

Every node spends 2 to 3.3% of wall time transmitting, nearly all of it
control traffic (control airtime is 97 to 100% of transmissions in every
run). The EU868 duty-cycle limit is 1%; no run would be legal there.

## Where the knee is

Between 10 and 50 nodes, and the mechanism is beacon saturation plus
discovery fragility:

1. **Beacons alone exceed channel capacity at 100+ nodes.** A beacon is
   608 ms on air; with the adaptive beacon controller averaging one beacon
   per node per ~40 s in these runs, 100 nodes offer 1.5 erlang and 200
   nodes 3.0 erlang of pure beacon traffic on a channel that saturates at
   1.0. Above ~100 nodes the medium is over capacity before any user
   traffic exists.
2. **Route discovery requires a multi-leg round trip through that storm.**
   An RREQ flood must reach the destination and a unicast RREP must hop back
   along the reverse path; with per-reception collision rates of 57 to 74%,
   the probability of completing both legs across multiple hops is small.
   The firmware retries a discovery twice (5 s, then 15 s, same query_id)
   and then gives up.
3. **Synchronized rebroadcasts make floods self-destructive.** Relays
   forward RREQs immediately on receipt with no jitter, so same-hop relays
   start transmissions at the same instant. LBT desyncs some of them (with
   LBT modeling disabled, the 10-node run delivers 2/20 instead of 5/20)
   but gives up after 3 attempts and transmits anyway.

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
  real SX126x CAD is preamble-biased and probabilistic.
- Overlap is computed on transmit windows; propagation offsets are ignored
  (microseconds against ToA of hundreds of milliseconds).
- Single channel, single SF; no inter-SF interference modeling.
- The simulator bridge still diverges from firmware orchestration in ways
  that flatter these results: it floods RREQs with hop limit 32 where the
  firmware caps at 4 (many scripted pairs in the 10/100/200-node grids are
  more than 4 hops apart, so the firmware as-built could not deliver them
  even on a clean channel), and reliability/receipt logic is a parallel
  implementation (convergence is workstream 2.5).

## Raw data

All runs are deterministic (fixed topologies, scripted traffic, seeded RNG
for SNR jitter and LBT backoff draws) and reproducible with the commands
above. The old-model rows reproduce at commit `d2483b9f`.
