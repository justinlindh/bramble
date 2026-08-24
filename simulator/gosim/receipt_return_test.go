package main

import "testing"

// TestReceiptReturnRateAllInRangeBroadcast is the receipt-return-rate
// metric's plumbing proof: a 3-node cluster where every node is in direct
// radio range of every other, A sends one broadcast, and both B and C
// (the only nodes other than the origin) store it and each send their own
// delivery receipt home (bridge.c's bridge_send_broadcast_delivery_receipt,
// mirroring firmware's queue_broadcast_delivery_receipt). With no relay
// hops and no contention, both receipts return: broadcast_receipts_expected
// counts the two (recipient, broadcast) pairs that owed a receipt,
// broadcast_receipts_registered counts the two the origin actually saw, and
// receipt_return_rate is exactly 1.0.
//
// This is deliberately a SMALL, uncontended mesh: it proves the metric's
// plumbing is wired correctly end to end, not that LBT deferral holds up
// under contention. A tiny mesh has no post-broadcast receipt storm, so
// 100% return here is expected and required; the contended storm scenario,
// where the blind-fire baseline is expected to fall short of 100%, is
// TestReceiptStormLBTDeferBeatsBlindFire's.
func TestReceiptReturnRateAllInRangeBroadcast(t *testing.T) {
	const scenarioJSON = `{
		"name": "receipt-return-3node-all-in-range",
		"mode": "deterministic",
		"duration_ms": 15000,
		"nodes": [
			{"id": "A", "x": 0,  "y": 0},
			{"id": "B", "x": 40, "y": 0},
			{"id": "C", "x": 0,  "y": 40}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "*"}
		]
	}`

	finalMetrics := runAndGetFinalMetrics(t, "receipt-return-3node-all-in-range", scenarioJSON)

	expected, _ := finalMetrics["broadcast_receipts_expected"].(float64)
	registered, _ := finalMetrics["broadcast_receipts_registered"].(float64)
	rate, _ := finalMetrics["receipt_return_rate"].(float64)

	if expected != 2 {
		t.Fatalf("broadcast_receipts_expected = %v, want 2 (B and C are the only non-origin "+
			"nodes and both are in range of A's broadcast)", finalMetrics["broadcast_receipts_expected"])
	}
	if registered != 2 {
		t.Fatalf("broadcast_receipts_registered = %v, want 2 (both B's and C's receipts should "+
			"reach A directly, one hop, no contention)", finalMetrics["broadcast_receipts_registered"])
	}
	if rate != 1.0 {
		t.Fatalf("receipt_return_rate = %v, want 1.0 (2/2)", rate)
	}
}

// TestReceiptReturnRateNoBroadcastsIsOne is the zero-denominator proof:
// a scenario that never broadcasts owes zero receipts, so receipt_return_
// rate must report 1.0 (nothing missed), not 0.0 (which would misread as
// total failure). Mirrors confirmedDeliveryRate/messageDeliveryRate's own
// documented zero-denominator handling, but with the opposite convention
// since those two really do mean "nothing happened" at 0.0, while an unused
// receipt-return metric means "nothing was owed".
func TestReceiptReturnRateNoBroadcastsIsOne(t *testing.T) {
	const scenarioJSON = `{
		"name": "receipt-return-no-broadcasts",
		"mode": "deterministic",
		"duration_ms": 15000,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "B"}
		]
	}`

	finalMetrics := runAndGetFinalMetrics(t, "receipt-return-no-broadcasts", scenarioJSON)

	expected, _ := finalMetrics["broadcast_receipts_expected"].(float64)
	registered, _ := finalMetrics["broadcast_receipts_registered"].(float64)
	rate, _ := finalMetrics["receipt_return_rate"].(float64)

	if expected != 0 {
		t.Fatalf("broadcast_receipts_expected = %v, want 0 (this scenario only sends unicast "+
			"DATA, never a broadcast)", finalMetrics["broadcast_receipts_expected"])
	}
	if registered != 0 {
		t.Fatalf("broadcast_receipts_registered = %v, want 0", finalMetrics["broadcast_receipts_registered"])
	}
	if rate != 1.0 {
		t.Fatalf("receipt_return_rate = %v, want 1.0 (0/0 owes nothing, misses nothing)", rate)
	}
}
