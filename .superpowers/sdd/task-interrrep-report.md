# Task: honest confirmed-delivery metric + intermediate-node RREP

Status: DONE

## Commits

On `feat/phase2-flood-comparison`:

- `b8caaab7` `feat(gosim): add honest reactive confirmed_delivery_rate metric` (Part A)
- `d3fd2185` `feat(routing): intermediate-node RREP to cut reactive discovery airtime` (Part B)

## Part A: honest reactive confirmed-delivery metric

### The gap

`message_delivery_rate` fires at DESTINATION RECEIPT: `bridge.c`'s
`bridge_msg_track_complete` is called from `_handle_data` the instant DATA
decodes at the destination ("don't wait for receipt to arrive at source",
`bridge.c` ~:845). Bramble's actual differentiator is confirmed delivery
(the originator learning its message got there), not mere reach. The whole
program had been citing this destination-reach number as if it were
sender-confirmation.

### The fix

`confirmed_delivery_rate` is defined as:

```
confirmed_delivery_rate = confirmed / (delivered + dropped + undelivered)
```

using the exact same terminal-state denominator `message_delivery_rate`
already uses, so the two are directly comparable side by side, reach vs.
confirmation, matching the same reach/confirm pairing flood mode already
reports as `flood_reached_rate` / `flood_confirmed_rate`.

`confirmed` counts distinct scripted messages whose delivery receipt made
it all the way back to the true ORIGINATOR. The detection point is
`_handle_delivery_receipt` (`simulator/gosim/bridge.c`), specifically the
branch `receipt.header.dest_addr == rx->addr`, the exact point that
already emits the hops-bearing `message_delivered` JSON event ("hops":N,
"path":[...]) only at the true originator (this is the analogous signal to
the flood-comparison work's "hops"-bearing event; for the reactive path it
lives in `_handle_delivery_receipt` rather than a broadcast-flood path).

New wiring:

- `metrics_state_t.confirmed_packets` (`simulator/engine/sim_metrics.h/.c`)
  plus `metrics_record_packet_confirmed`.
- `msg_tracker_t.confirmed` (`simulator/gosim/bridge.h`), reset on slot
  reuse in `bridge_msg_track_add`.
- `bridge_msg_track_confirm` (`bridge.c`): looks the tracker entry up by
  `packet_id`, regardless of `active` (the entry is already inactive by
  the time a receipt returns, since `bridge_msg_track_complete` deactivated
  it at DATA arrival), and de-dupes on its own `confirmed` flag, so two
  receipts for the same message (e.g. both of two DATA retries succeeding)
  are not double-counted. Called from `_handle_delivery_receipt` right
  where the receipt reaches the originator.
- `simulator/gosim/sim.go`'s `complete()` emits `confirmed` and
  `confirmed_delivery_rate` in `final_metrics`, alongside the unchanged
  `delivered` / `message_delivery_rate`.

### Tests (gosim, `go test -count=1 ./...`, all pass)

- `TestConfirmedDeliveryRateMatchesReceiptsHome`: 4-hop line (A-B-C-D), one
  scripted message. The receipt returns (Phase 1's breadcrumb fix), so
  `confirmed_delivery_rate == message_delivery_rate == 1.0` (1/1).
- `TestConfirmedDeliveryRateHonestUnderLoad`: 50-node SF7/250kHz dense grid
  (45u spacing) under legacy 2 msg/min traffic. Real, reproducible run:
  `delivered=6` (`message_delivery_rate=0.3`), `confirmed=5`
  (`confirmed_delivery_rate=0.25`), strictly less, proving a message can
  reach its destination without its receipt finding its way home under
  load. Asserts the exact `confirmed/total` arithmetic too.

## Part B: intermediate-node RREP

### The problem

Every RREQ floods the whole mesh to find its destination, even when an
intermediate relay already has a fresh route to that destination cached.
This is reactive routing's dominant airtime cost at scale (per this
branch's prior SF7/discovery-saturation findings).

### The rules (`components/routing/include/discovery.h` /
`discovery.c`)

`intermediate_rrep_route_usable(route, now_ms)`, deliberately more
conservative than classic AODV, because Bramble's `route_entry_t` carries
no destination sequence number (only `metric`/`hop_count`/`state`), so
AODV's normal per-destination freshness check is unavailable:

- Trust class: only `ROUTE_SRC_DISCOVERED` (RREQ/RREP/beacon, HMAC-gated
  control plane) qualifies. A `ROUTE_SRC_BREADCRUMB` route (learned off a
  forwarded DATA frame's `prev_hop`, which is relay-mutable and
  MAC-excluded, an unauthenticated hint) is never eligible: letting a
  breadcrumb author a reply on someone else's behalf would let a
  bystander plant/replay a breadcrumb-looking path and turn every relay
  into an open blackhole oracle for any destination it names.
- State: only `ROUTE_ACTIVE`. `STALE`/`BROKEN` routes never reply.
- Freshness: `last_confirmed` must be within `INTERMEDIATE_RREP_MAX_AGE_MS`
  (60s), tighter than the 5-minute `ROUTE_ACTIVE_TIMEOUT_MS` active-to-stale
  transition, since that state transition alone is too coarse a stand-in
  for a destination sequence number. A wrong intermediate reply, unlike a
  wrong forward, actively terminates the real flood's chance of reaching
  the true destination down that entire subtree, so this errs conservative
  at the cost of some intermediate-reply coverage.

Forward-or-not: the node REPLIES and does NOT also forward the RREQ. This
is the airtime-saving half of the tradeoff, since the whole point of this
feature is to cut RREQ flood cost, not just add RREP traffic on top of an
unchanged flood. It is safe because the RREQ is a broadcast: every other
neighbor that heard the same RREQ still makes its own independent forward
decision, so a subtree this one node cannot vouch for still floods through
other paths, and the originator's expanding-ring retry (a fresh query_id)
still reaches D normally if this node's cached route turns out to be
wrong.

Hop/metric math (`rrep_build_intermediate`): `hop_count` follows the
existing codebase's established convention (verified against
`rrep_build_destination`/`test_three_node_discovery`) that hop_count is
the frozen total path length from the ORIGINAL query's originator, not a
live per-node graph distance; every relay along a route installs the same
historical number, not its own true distance. Consistent with that:
`hop_count = (rreq->hop_count + 1) + route_to_dest->hop_count` (hops
originator-to-me, plus my cached route's stored hop_count for D). This is
an honest, documented approximation, not an exact minimum-hop count; it
can over-state the true distance, which is the safe direction to err (an
inflated hop_count only makes the path look less attractive in downstream
tie-breaks, never spuriously wins one). `route_metric` composes two
independently-255-baselined path-quality scores by treating `metric` as
"255 minus accumulated link penalties" throughout (never a min or an
average): `metric_to_me` extends the RREQ's originator-to-prev_hop score
across the last link (identical formula to `rreq_forward`), then the
cached route's own penalty (`255 - route_to_dest->metric`) subtracts
further, floored at zero.

### Auth confirmation

`rrep_build_intermediate` calls `rrep_sign` exactly like
`rrep_build_destination` does. The intermediate RREP carries the same
`auth_hmac[8]` over the same origin-stable fields
(`query_id`/`src_addr`/`hop_count`/`route_metric`), because a replying
relay is a new RREP source under the identical trust rules as any real
destination. Host test `test_rrep_build_intermediate_hop_and_metric_math`
asserts `rrep_verify(&rrep)` succeeds; `test_rrep_build_intermediate_tamper_fails_verify`
proves a MAC-covered field mutation (hop_count) fails verification.

### Wiring

- `main/mesh_task.c`'s `handle_rreq`: always on (shipped firmware
  behavior). Board build (Heltec V3, ESP-IDF 5.4) compiles clean.
- `simulator/gosim/bridge.c`'s `_handle_rreq`: mirrors the firmware using
  the SAME `intermediate_rrep_route_usable`/`rrep_build_intermediate`
  functions, so the sim cannot drift from firmware's trust/freshness
  rules. Adds `bridge_set_intermediate_rrep_enabled`/scenario field
  `"intermediate_rrep"` (default true) purely so a gosim scenario can A/B
  the feature on identical topology/traffic for measurement; firmware has
  no such switch. `simulator/scenarios/generate.py` gained
  `--intermediate-rrep {0,1}`.

### Host test results (`bash test/run_all_tests.sh`, 92/92 suites, 0
failures)

New tests in `test/test_discovery.c` (all pass):

- `test_intermediate_rrep_route_usable_accepts_fresh_discovered_active`
- `test_intermediate_rrep_route_usable_rejects_null`
- `test_intermediate_rrep_route_usable_rejects_breadcrumb`
- `test_intermediate_rrep_route_usable_rejects_stale_and_broken`
- `test_intermediate_rrep_route_usable_rejects_too_old`
- `test_rrep_build_intermediate_hop_and_metric_math`
- `test_rrep_build_intermediate_tamper_fails_verify`
- `test_intermediate_reply_suppresses_forward` (integration: mirrors
  `handle_rreq`'s control flow, proves the reply-instead-of-forward
  decision produces a valid signed RREP with `forwarded == false`)

### gosim parity test results (`go test -count=1 ./...`, all pass)

`simulator/gosim/intermediate_rrep_test.go`, star topology (I hub, D/E/F
spokes each in range of I only): E discovers D first (I has no cached
route yet, floods normally); F then discovers D while I's route to D is
still fresh.

- `TestIntermediateRREPShortCircuitsSecondDiscovery` (enabled, the
  default): both messages delivered AND confirmed (`delivered=2`,
  `confirmed=2`), `rreqs_sent < 8`, F's discovery is short-circuited at I.
- `TestIntermediateRREPDisabledFloodsFully` (`intermediate_rrep:false`):
  identical delivery/confirmation correctness, but `rreqs_sent >= 8`, F's
  RREQ floods all the way to D and back, same as any ordinary discovery.
  This is both the toggle's own correctness proof and the "before"
  baseline.

Also verified: `bash scripts/check-rpc-contract.sh` (51/51), clang-format
v14 clean on every touched C/H file, `go vet ./...` clean.

## MEASURE: before/after at the honest SF-range-coupled radio model

Generated with `simulator/scenarios/generate.py` (`--intermediate-rrep
{0,1}`), run headless (`bramble-gosim --headless --scenario ...`), reading
each run's `final_metrics` JSON event. All runs are `mode: deterministic`
and bit-for-bit reproducible (verified by re-running).

Both scenarios: 50 nodes, grid topology, legacy 2 msg/min traffic (20
scripted messages), 600s duration. Flood benchmark: `--routing flood
--flood-hop-limit 7` (Bramble's `ROUTE_HOP_LIMIT_MAX`-matched, the fair
basis per this branch's prior flood-comparison work).

### (1) SF10/125kHz, 120-unit grid spacing (the standard saturated baseline)

| | before (off) | after (on) | flood (hl=7) |
|---|---|---|---|
| reach (message_delivery_rate) | 0% (0/20) | 0% (0/20) | 60% (12/20) |
| confirmed (confirmed_delivery_rate) | 0% (0/20) | 0% (0/20) | 30% (6/20) |
| messages_sent (of 20 scripted) | 1 | 1 | 20 (flood is send-and-forget) |
| RREQ airtime (`airtime_ms_by_type.rreq`) | 676,614 ms | 676,614 ms (unchanged) | n/a (flood has no discovery) |

Intermediate-RREP does NOT help here. Flagging honestly, this is real
data. The before/after runs are bit-for-bit identical. At this level of
saturation (offered load 1.68 erlangs from RREQ traffic alone,
`channel_util_pct` 168%), only a single scripted message ever even
attempts discovery in the whole 600s window (`messages_sent=1` in both
runs) before the channel is completely jammed; every other scripted
message never gets far enough to originate its own RREQ at all. Since
intermediate-RREP only pays off when a second, later discovery for the
same destination arrives at a node that already cached a route from an
earlier discovery, and this scenario never gets past its first discovery,
there is no second beneficiary event for the mechanism to short-circuit.
The bottleneck here is that the channel is saturated before discovery can
even complete once, not that discovery is repeated wastefully, a
different failure mode than the one this lever targets.

### (2) SF7/250kHz, 45-unit grid spacing (the connected dense regime)

| | before (off) | after (on) | flood (hl=7) |
|---|---|---|---|
| reach (message_delivery_rate) | 30% (6/20) | 65% (13/20) | 60% (12/20) |
| confirmed (confirmed_delivery_rate) | 25% (5/20) | 55% (11/20) | 30% (6/20) |
| messages_sent (of 20 scripted) | 8 | 15 | 20 |
| RREQ airtime (`airtime_ms_by_type.rreq`) | 61,828 ms | 56,577 ms (-8.5%) | n/a |
| rreqs_sent | 1,719 | 1,573 | n/a |
| offered_load_erlangs | 0.155 | 0.168 | n/a |

Intermediate-RREP is a clear win here. Reach more than doubles (30% to
65%) and confirmed delivery more than doubles (25% to 55%), while RREQ
airtime and RREQ packet count both go down (-8.5% airtime, -146 RREQ
transmissions): the mechanism cuts cost and improves outcomes
simultaneously, because completed discoveries no longer have to re-flood
for destinations a relay already knows a fresh route to; the freed
capacity lets more of the 20 scripted messages get far enough to even
originate a DATA send (`messages_sent` 8 to 15).

Against the flood benchmark, this is the headline finding: with
intermediate-RREP, reactive routing now beats flood on both bars in the
dense regime, reach 65% vs flood's 60%, and confirmed delivery 55% vs
flood's 30% (nearly double flood's confirmed rate). Before this change,
reactive trailed flood on reach (30% vs 60%) and roughly matched it on
confirmed (25% vs 30%). This directly strengthens the prior "reactive is
salvageable in dense deployments" finding (progress.md) into "reactive can
outright win in dense deployments" once this lever is in place.

### Honest summary

Intermediate-node RREP is a real, mechanism-proven airtime lever (RREQ
airtime and RREQ count both drop wherever it has anything to do), but its
payoff is conditional: it only helps once a node has survived long enough
to complete at least one earlier discovery for a repeat destination. In
the already-catastrophically-saturated SF10/120u baseline, the channel
never lets even a single discovery complete, so the lever has nothing to
work with, a genuinely different bottleneck than the one it targets, and
this branch's existing SF7-dense-deployment lever (or the not-yet-explored
RREQ-rate-limiting/backoff levers) is likely a prerequisite before
intermediate-RREP's benefit is visible at that scale/spacing. In the
dense, unsaturated regime, it delivers exactly the improvement it was
designed for, and enough of one that reactive routing overtakes managed
flooding on both reach and confirmed delivery.
