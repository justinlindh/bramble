package main

import (
	"fmt"
	"math"
	"strings"
	"testing"
)

// receiptStormNodes is the storm's mesh size: one origin plus nine hearers,
// so a single broadcast owes exactly nine delivery receipts. Every node is in
// direct radio range of every other (see receiptStormScenarioJSON), which is a
// hard requirement, not a convenience: gosim's broadcast receipts do not
// propagate past the first hop under the default reactive routing (see
// bridge_send_broadcast_delivery_receipt's "Known gap" comment in bridge.c),
// so a multi-hop storm topology would measure that gap instead of contention.
const receiptStormNodes = 10

// receiptStormSeeds is the number of seeds each arm runs. Ten is enough to
// separate the arms here (the per-seed table in the header comment shows the
// spread) without turning one test into a minute of wall clock. Both arms
// over all ten seeds run in just over a second (`go test -run
// TestReceiptStormLBTDeferBeatsBlindFire -v`, 2026-07-28), so this stays at
// ten rather than dropping to a smaller count for CI speed: the means are
// stable at this count (see the calibration table below) and a smaller
// sample would buy no runtime that matters while making the >= 0.95 gate
// more sensitive to which seeds happen to be included.
const receiptStormSeeds = 10

// receiptToaTolerancePct bounds how much the defer arm's receipt-tier ToA may
// exceed the blind-fire arm's before the airtime-cost regression check below
// fails. Calibration measured the two arms' receipt-tier ToA as bit-identical
// (7658ms) on every one of the ten seeds: the fix changes WHEN a receipt
// transmits (deferred off a busy channel, replayed later), never HOW MANY
// transmit or at what frame size, so the packet count and per-packet size
// behind this figure do not depend on RNG state or which seed runs. A tight
// zero-tolerance check would therefore be defensible today, but this test
// runs in CI forever and a future change to receipt framing, SF, or the
// storm topology could introduce a few milliseconds of legitimate rounding
// noise between arms without that being a regression. 2% is loose enough to
// absorb that and tight enough that it would still catch the fix regressing
// to spending materially more air than it did at calibration.
const receiptToaTolerancePct = 0.02

// receiptStormScenarioJSON builds one storm run: receiptStormNodes nodes
// packed inside a single radio cell, left alone long enough for beacons to
// populate every neighbor table, then one broadcast from N0.
//
// Why the long lead-in: the receipt anti-collision slot window is sized from
// the sender's peer count (mesh_broadcast_receipt_slot_delay_ms clamps
// 2 * peer_count into [4, 32] buckets), so a node that has not yet heard its
// neighbors would spread its receipts over the 4-bucket minimum and overstate
// the collision rate. Beacons run on a ~60s cadence, so the broadcast fires at
// 150s with the mesh fully formed.
//
// Positions: a jittered ring plus one node off-center, so the receipts arrive
// at the origin with a spread of RSSIs. Equidistant nodes would make every
// overlap a mutual kill, since the capture effect needs a >= 6 dB winner
// (simulator/engine/sim_radio.c's radio_check_reception); real hearers are not
// equidistant, and pinning them all at one radius would bias the measurement
// toward the defect.
func receiptStormScenarioJSON(seed int, txKind string) string {
	// Radii chosen to sit well inside range 150 (max node-to-node distance is
	// under 130) while spanning enough path loss for capture to matter.
	radii := []float64{18, 34, 47, 55, 61, 22, 40, 52, 64}
	var nodes []string
	nodes = append(nodes, `{"id": "N0", "x": 0, "y": 0}`)
	for i, r := range radii {
		// Golden-angle placement: no two hearers share a bearing, and the
		// pattern is fixed across seeds so only the RNG differs between runs.
		angle := 2.39996 * float64(i+1)
		x := r * math.Cos(angle)
		y := r * math.Sin(angle)
		nodes = append(nodes, fmt.Sprintf(`{"id": "N%d", "x": %.2f, "y": %.2f}`, i+1, x, y))
	}

	return fmt.Sprintf(`{
		"name": "receipt-storm-10node",
		"mode": "deterministic",
		"seed": %d,
		"duration_ms": 180000,
		"receipt_tx_kind": "%s",
		"nodes": [%s],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 150000, "type": "send_message", "src": "N0", "dest": "*"}
		]
	}`, seed, txKind, strings.Join(nodes, ", "))
}

// receiptStormResult is one seed's outcome for one arm. The two airtime
// figures come from the sim's per-type time-on-air accounting, charged once
// per real transmission inside sim_radio_broadcast_lbt: receiptToaMs is the
// RECEIPT lane's share, totalToaMs the whole mesh's. They belong beside the
// return rate because the fix has to buy its reliability without spending
// more air: a deferred attempt is never transmitted and so never charged, and
// every collision it avoids is airtime that would have been wasted.
type receiptStormResult struct {
	rate         float64
	receiptToaMs float64
	totalToaMs   float64
}

// runReceiptStorm runs one arm over receiptStormSeeds seeds. It fails the test
// if the metric plumbing does not hold on every run: nine receipts owed (one
// per hearer), a rate inside [0, 1], registered never exceeding expected, and
// a nonzero RECEIPT-lane ToA (receipts that never reached the real airtime
// accounting would make the airtime half of this comparison meaningless).
func runReceiptStorm(t *testing.T, txKind string) []receiptStormResult {
	t.Helper()
	results := make([]receiptStormResult, 0, receiptStormSeeds)
	for seed := 1; seed <= receiptStormSeeds; seed++ {
		name := fmt.Sprintf("receipt-storm-%s-seed%d", txKind, seed)
		fm := runAndGetFinalMetrics(t, name, receiptStormScenarioJSON(seed, txKind))

		expected, _ := fm["broadcast_receipts_expected"].(float64)
		registered, _ := fm["broadcast_receipts_registered"].(float64)
		rate, _ := fm["receipt_return_rate"].(float64)
		totalToa, _ := fm["airtime_total_ms"].(float64)
		byType, _ := fm["airtime_ms_by_type"].(map[string]any)
		receiptToa, _ := byType["receipt"].(float64)

		if expected != receiptStormNodes-1 {
			t.Fatalf("%s: broadcast_receipts_expected = %v, want %d (every node but the "+
				"origin is in range and must store the broadcast)", name, fm["broadcast_receipts_expected"],
				receiptStormNodes-1)
		}
		if registered > expected {
			t.Fatalf("%s: broadcast_receipts_registered = %v exceeds expected = %v; the "+
				"(orig_packet_id, recipient) dedup is not holding", name, registered, expected)
		}
		if rate < 0.0 || rate > 1.0 {
			t.Fatalf("%s: receipt_return_rate = %v is outside [0, 1]", name, rate)
		}
		if want := registered / expected; rate != want {
			t.Fatalf("%s: receipt_return_rate = %v, want registered/expected = %v", name, rate, want)
		}
		if receiptToa <= 0 {
			t.Fatalf("%s: airtime_ms_by_type[receipt] = %v, want > 0: nine receipts were owed, "+
				"so the RECEIPT lane must have been charged for the ones that flew", name, receiptToa)
		}
		results = append(results, receiptStormResult{
			rate: rate, receiptToaMs: receiptToa, totalToaMs: totalToa,
		})
	}
	return results
}

// receiptStormMeans averages an arm's three tracked figures.
func receiptStormMeans(rs []receiptStormResult) receiptStormResult {
	var sum receiptStormResult
	for _, r := range rs {
		sum.rate += r.rate
		sum.receiptToaMs += r.receiptToaMs
		sum.totalToaMs += r.totalToaMs
	}
	n := float64(len(rs))
	return receiptStormResult{
		rate: sum.rate / n, receiptToaMs: sum.receiptToaMs / n, totalToaMs: sum.totalToaMs / n,
	}
}

// TestReceiptStormLBTDeferBeatsBlindFire is the receipt reliability campaign's
// Task 2 calibration: a ten-node all-in-range cluster where one broadcast
// makes nine nodes answer the same origin at once, run twice over the same ten
// seeds with one parameter changed.
//
// The two arms are both real firmware kinds, so the arm switch is a parameter
// and not a second implementation. An originated broadcast delivery receipt
// goes out as TX_KIND_RECEIPT, which components/radio/tx_gate.c's lbt_defers()
// answers true for: three busy CAD checks and the send is handed back to the
// app layer (TX_GATE_ERR_CHANNEL_BUSY) to retry later. TX_KIND_RECEIPT_FORWARD
// is not in that set, so it keeps the behavior every kind had before the fix:
// after three busy CAD checks, transmit anyway. Same code, same seeds, one
// parameter, and that parameter is exactly the defect.
//
// Both arms carry the full app-layer receipt policy from
// main/broadcast_delivery_receipt.c and main/mesh_reliability.c (deterministic
// slot delay, jitter, three attempts, scaled inter-attempt backoff), because
// that policy exists in the pre-fix world too. Only the busy-channel decision
// differs.
//
// Calibration measured 2026-07-28 on this scenario (go test -run
// TestReceiptStormLBTDeferBeatsBlindFire -v), receipt_return_rate per seed:
//
//	seed:        1      2      3      4      5      6      7      8      9     10    mean
//	blind-fire  .8889 1.0000 .8889 .6667 .8889 .5556 .8889 .8889 .8889 .8889  .8444
//	defer       1.0000 1.0000 1.0000 .8889 1.0000 1.0000 1.0000 1.0000 1.0000 1.0000  .9889
//
// The blind-fire arm loses 15.6% of broadcast receipts on average, which
// reproduces the defect (the acceptance bar was a mean under 0.95). It is a
// milder loss than the bench's traced 20-25%: this sim runs the freq_plan
// default SF9 rather than the bench's SF10, so a receipt occupies ~284ms of
// air here instead of the ~150-200ms-at-SF10 figure
// broadcast_delivery_receipt.c cites, against slot spacing that does not
// change with SF. Reported as measured, not tuned toward the bench number.
//
// Airtime, same runs, identical on every seed in both arms (all counts here
// are seed-independent: 27 receipt transmissions, nine hearers times three
// attempts, plus a fixed beacon cadence and flood):
//
//	receipt-tier ToA  7658ms per arm      total ToA  21622ms per arm
//
// That equality is the point rather than a coincidence: no attempt in the
// defer arm ever hit the eight-defer cap, so the same 27 transmissions flew in
// both arms, merely at quieter instants. The defer arm buys 14.4 points of
// receipt return for exactly zero extra air.
//
// HISTORY: the original calibration (tight 500ms slot pitch, short fixed
// retry backoffs) reproduced the bench defect at blind-fire mean 0.8444 vs
// defer 0.9889. Bench telemetry then showed the dominant loss was storm
// WINDOW OCCUPANCY, not blind-fire: with the slot pitch widened to 1000ms
// and retries re-drawing full attempt-salted slots, BOTH arms saturate near
// 1.0 in this scenario, exactly as the occupancy analysis predicts. The
// blind-fire-reproduces-the-defect assertion is therefore retired; the
// scenario now gates the shipping configuration.
//
// This test asserts the metric plumbing on every run plus the campaign's
// exit gate: the shipping (defer) arm clears mean rate >= 0.95, is never
// worse than the blind-fire reference beyond seed noise, and does not buy
// its reliability with extra receipt-tier airtime (within
// receiptToaTolerancePct of the blind-fire arm's ToA).
func TestReceiptStormLBTDeferBeatsBlindFire(t *testing.T) {
	blindFire := runReceiptStorm(t, "receipt_forward")
	deferring := runReceiptStorm(t, "receipt")

	for i := range blindFire {
		t.Logf("seed %2d | blind-fire rate %.4f receipt_toa %6.0fms total_toa %6.0fms "+
			"| defer rate %.4f receipt_toa %6.0fms total_toa %6.0fms",
			i+1, blindFire[i].rate, blindFire[i].receiptToaMs, blindFire[i].totalToaMs,
			deferring[i].rate, deferring[i].receiptToaMs, deferring[i].totalToaMs)
	}
	blindMean := receiptStormMeans(blindFire)
	deferMean := receiptStormMeans(deferring)
	t.Logf("mean    | blind-fire rate %.4f receipt_toa %6.0fms total_toa %6.0fms "+
		"| defer rate %.4f receipt_toa %6.0fms total_toa %6.0fms",
		blindMean.rate, blindMean.receiptToaMs, blindMean.totalToaMs,
		deferMean.rate, deferMean.receiptToaMs, deferMean.totalToaMs)

	if deferMean.rate < blindMean.rate-0.02 {
		t.Fatalf("defer arm mean receipt_return_rate = %.4f is worse than the blind-fire "+
			"reference's %.4f beyond seed noise; deferring off a busy channel must never "+
			"return fewer receipts than transmitting into it", deferMean.rate, blindMean.rate)
	}
	if deferMean.rate < 0.95 {
		t.Fatalf("defer arm mean receipt_return_rate = %.4f, want >= 0.95: this is the receipt "+
			"reliability campaign's exit gate, not just an improvement over blind-fire", deferMean.rate)
	}
	if maxToa := blindMean.receiptToaMs * (1 + receiptToaTolerancePct); deferMean.receiptToaMs > maxToa {
		t.Fatalf("defer arm mean receipt-tier ToA = %.1fms exceeds the blind-fire arm's %.1fms "+
			"by more than %.0f%%: the defer fix must not spend more airtime to reach its higher "+
			"return rate, and a deferred attempt should never be charged more than a blind-fired "+
			"one that flew at the same size", deferMean.receiptToaMs, blindMean.receiptToaMs,
			receiptToaTolerancePct*100)
	}
}
