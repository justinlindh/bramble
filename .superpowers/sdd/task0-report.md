# Phase 2 Task 0: flood-comparison baseline -- report

Status: **DONE**

Commit: `cd7daa71` "feat(gosim): Meshtastic-style managed-flooding routing
mode (Phase 2 Task 0)" on branch `feat/phase2-flood-comparison` (worktree
`/home/justin/src/worktrees/phase2-scale`, base `372aadfa`).

Build/test: `go build` clean, `go vet` clean, `gofmt -l` clean, `go test
-count=1 ./...` green (29 tests, including 2 new flood-specific tests), and
the firmware host suite (`bash test/run_all_tests.sh`) unaffected: 92/92
test suites still pass (no firmware C touched by this task).

Raw data + full observations: `task0-flood-comparison-data.md` (this
directory, not committed per the coordination convention). This report is
the short-form summary requested.

## The flood algorithm as implemented

Go-only (`simulator/gosim/flood.go`), selected per-scenario via
`"routing":"flood"` (default `"reactive"`, Bramble's real firmware AODV
path via `bridge_handle_*`, unmodified). Bypasses `bridge_handle_generate_
message`/`bridge_handle_receive_packet` entirely for flood scenarios (own
`EVT_GENERATE_MESSAGE`/`EVT_RECEIVE_PACKET`/`EVT_SEND_PACKET` handlers,
dispatched by routing mode in `sim.go`), but every transmission still goes
through the exact same `C.sim_radio_broadcast` chokepoint and the exact
same `airtime_budget_can_transmit`/`_debit` (BROADCAST tier) every other TX
site in the sim uses, so collision/capture/ToA/airtime accounting is
identical to reactive's:

- **Dedup**: `(packet_id, src_addr)`, checked per-receiving-node, exactly
  as specified.
- **Hop limit**: default 3 (Meshtastic's shipped default), also run at 7
  (Meshtastic's max) in the sweep; decremented once per relay hop (the
  origin's own transmission does not decrement it), no route discovery, no
  reverse routes, fire-and-forget.
- **Rebroadcast delay**: SNR-derived -- weaker SNR (0 dB) gets the shortest
  delay (100ms + 0-49ms jitter), the strongest SNR seen (20+ dB, clamped)
  gets the longest (500ms + jitter), so the hearer with the least redundant
  coverage tends to key up first. This is a **documented approximation** of
  Meshtastic's actual contention-window heuristic, not a port of its
  firmware source; the constants (`floodRebroadcastBaseMs`/`SpreadMs`/
  `MaxSNRdB` in flood.go) are named and tunable, not hidden magic numbers.
- **Duplicate suppression**: a node cancels its own pending rebroadcast the
  first time (`floodSuppressAfterHeard = 1`, Meshtastic's own threshold) it
  overhears another relay of the same `(packet_id, src_addr)` before its
  own delay elapses. Measurably active in the sweep: 25-27% of scheduled
  relays were cancelled at 50/100 nodes (hop_limit=7).
- **Confirmation (this task's addition, not in the brief verbatim, needed
  for the strict bar)**: the destination originates a Meshtastic-style
  flooded ACK back toward the sender -- itself hop-limited, deduped, fire-
  and-forget, no reverse route, exactly the same mechanics as the DATA it
  acks (this is genuinely how Meshtastic's `want_ack` works, not an
  invented mechanism). Without this, flood's strict bar would be
  structurally N/A; the honest-comparison framework explicitly allows this
  choice ("OR explicitly N/A with the reason, do the honest thing and
  document which") -- I chose to model it because Meshtastic really does
  have this feature, so N/A would have undersold flood's real capability.
- **Approximations/what I did NOT model**: no periodic beacons/NodeInfo/
  position broadcasts in flood mode (managed flooding has no neighbor
  table to maintain, so there is nothing for one to serve; `EVT_TICK_NODE`
  is a no-op in flood mode). No SF7/preset differences, no real Meshtastic
  firmware timing constants. Frame sizes: flood DATA/ACK = 18 bytes
  (type+hop_limit+packet_id+src+dest[+corr_id]), reactive's header-only
  legacy DATA = 12 bytes (`HEADER_SIZE`) -- close enough for a fair ToA
  comparison, exact bytes documented in flood.go's comments.

## Head-to-head table (both bars, both transports, 10/50/100 nodes)

Full table with airtime breakdown in `task0-flood-comparison-data.md`.
Summary (LOOSE = destination reach, no confirmation; STRICT = confirmed
delivery back to sender):

| Nodes | Reactive LOOSE/STRICT | Flood hl=3 LOOSE/STRICT | Flood hl=7 LOOSE/STRICT | Winner |
|---|---|---|---|---|
| 10  | 50% / 40%   | 100% / 100% | 100% / 100% | **Flood**, both hop limits, by a wide margin |
| 50  | 10% / 0%    | 0% / 0% (hop limit too short for this grid's diameter) | 60% / 30% | **Flood** (hl=7) |
| 100 | 1.7% / 0%   | 0% / 0% (same reason) | 90% / 90% | **Flood** (hl=7), decisively |

**Flood wins at every node count, on both bars, whenever its hop limit can
topologically reach the destination.** hop_limit=7 (matched to Bramble's
own `ROUTE_HOP_LIMIT_MAX=8`) is the fair comparison basis at this grid
size; hop_limit=3 fails to connect at 50/100 nodes for a reason that has
nothing to do with routing strategy (topology diameter exceeds the hop
budget), which is itself a real, reportable finding, not a footnote.

**Why (the airtime reason)**: reactive's control plane is airtime-bankrupt
at scale. At 50 nodes RREQ alone burns 682 of the 600 real-time seconds of
ToA (over 1.0 erlang from discovery alone, before any DATA gets a fair shot
at the channel); at 100 nodes it's 697s RREQ + 645s beacons against the
same 600s window. Flood (hl=7) tops out at 0.59 and 1.03 offered erlangs
respectively at the same node counts -- still oversubscribed at 100 nodes,
but by a much smaller margin, and every erlang it spends is on the message
itself or its ACK, never on finding a route (zero discovery cost by
construction). Reactive re-floods RREQ from scratch per cold discovery
attempt; flood's relay cost is bounded per message by hop_limit and cut
further by duplicate suppression.

## This is the project's central-thesis-questioning finding

Per the framework's own instructions, I'm reporting this plainly and not
softening it: **naive managed flooding beats Bramble's reactive AODV
routing at every node count measured in this honest simulator, under
identical modeled radio physics**, whenever flood's hop budget can
topologically reach the destination. This is a simulated-strategy
comparison on identical physics, not a product benchmark against real
Meshtastic firmware (which has years of RF tuning this sim-layer model does
not capture) -- the correct phrasing is "reactive routing delivered X vs
managed-flooding Y under identical modeled conditions," not "Bramble beats
Meshtastic" or its inverse. But within that scope, the result is
unambiguous and is exactly the D2-class finding the framework anticipated:
it questions whether reactive routing is the right architectural bet at
this traffic rate/topology, or whether a hybrid/flood-for-small-mesh
approach should be considered. I am not recommending which; that is an
owner decision per the framework's D2 branch.

## An additional finding surfaced along the way (not part of the ask, but load-bearing)

Investigating what "confirmed" should mean for flood's strict bar led me to
discover that reactive's own existing `final_metrics.message_delivery_rate`
field -- the number every prior Phase 0/1/2 document calls
"delivery-with-confirmation" and treats as THE headline metric -- is
actually measured at **destination receipt** (`bridge.c`'s `_handle_data`
comment literally says "Record delivery immediately, don't wait for
receipt to arrive at source"), not at confirmed return to the sender. I
built a genuinely stricter measurement for this task (scanning the JSON
event stream for the `"hops"`-bearing `message_delivered` event that
`_handle_delivery_receipt` only emits when it reaches the true originator)
and it diverges sharply from the existing metric: 40% vs the existing
metric's 50% at 10 nodes, and **0% vs ~10%/~1.7%** at 50/100 nodes -- at
scale, literally none of reactive's "delivered" messages in this sweep ever
had their confirmation return to the sender. This is a pre-existing
property of the current codebase (I changed nothing on the reactive path),
not a bug this task introduced, but it means the honest baseline document's
"delivery-with-confirmation" framing at 50/100 nodes should be read as
"destination reach," not "confirmed delivery." Flagging for the controller
to decide whether `docs/results/simulation-2026-07-honest-baseline.md`
needs a correction pass; out of scope for me to fix here.

## Confirmation: identical physics

Both transports go through the same `C.sim_radio_broadcast` (ToA,
half-duplex serialization, LBT, channel occupancy log, propagation delay,
RSSI/SNR), the same `radio_check_reception` (collision/capture/half-duplex
outcomes), and the same `airtime_budget_can_transmit`/`_debit` (BROADCAST
tier for flood, matching reactive's own `TX_KIND_DATA_BROADCAST` ->
`AIRTIME_TIER_BROADCAST` convention). Same grid topology/spacing (120
units, ~4 neighbors), same SF10/125kHz, same 2 msg/min traffic, same 3
seeds, same 600s duration, same scripted src/dest message list (the
scenario JSON differs ONLY in its `"routing"` field). No parallel physics
implementation exists anywhere in flood.go.

## Concerns / follow-ups for the controller

1. The confirmed-delivery caveat above (reactive's `message_delivery_rate`
   is destination-reach, not sender-confirmed) affects how every PRIOR
   baseline number in this program should be read, not just this task's
   output. Worth a decision on whether to correct the historical doc.
2. hop_limit sensitivity at this grid size is steep (0% at hl=3 vs
   60-90% at hl=7, at 50/100 nodes) -- if a future lever sweep varies
   topology/spacing, flood's hop_limit should probably scale with it too,
   or the comparison stops being apples-to-apples.
3. I did not attempt to model Meshtastic's real preset/SF choices, tx power,
   or duty-cycle behavior differences -- purely a routing-strategy
   comparison on Bramble's own radio defaults, as scoped.
