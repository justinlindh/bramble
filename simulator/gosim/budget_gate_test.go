package main

import "testing"

// TestBeaconBudgetGateDeniesAndCountsOnceExhausted pins node_tick's beacon TX
// to the real airtime budget (airtime_budget_can_transmit /
// airtime_budget_debit), the same component firmware's tx_gate uses, rather
// than letting it transmit unconditionally.
//
// Setup: a tiny 2-node scenario (A transmits, B is a silent neighbor in
// range) with A's BROADCAST lane shrunk to less than two beacons' worth of
// airtime. A's first beacon is forced due immediately so it fires
// deterministically; ticking forward to each subsequent scheduled beacon
// must eventually hit a tick where the lane can't cover the cost, at which
// point the gate must deny (no packet on the air) and record it in
// budget_denied[BROADCAST] rather than transmitting for free.
func TestBeaconBudgetGateDeniesAndCountsOnceExhausted(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addrA = 0x000000A0
	const addrB = 0x000000B0
	h.addNode(addrA, 0, 0)
	h.addNode(addrB, 10, 0)

	a := h.activateNode(addrA)
	h.activateNode(addrB)

	beaconMs := uint32((h.frameAirtimeUs(beaconWireSize()) + 999) / 1000)
	if beaconMs == 0 {
		beaconMs = 1
	}

	// Enough for one beacon plus a sliver, not two: with a lane this small
	// the hourly refill between 60s-ish beacon intervals is negligible, so
	// the second attempt must be denied.
	h.setBroadcastBudgetMs(a, beaconMs+beaconMs/2)
	h.forceBeaconDue(a, 0)

	now := uint64(0)
	sent := 0
	for i := 0; i < 5; i++ {
		if n := h.tick(a, now); n > 0 {
			sent++
		}
		next := h.nextBeaconDue(a)
		if next <= now {
			next = now + 70_000_000 // fallback: force progress if unchanged
		}
		now = next
	}

	if sent == 0 {
		t.Fatalf("expected at least one beacon to succeed before the budget exhausted")
	}
	if sent >= 5 {
		t.Fatalf("expected the tiny budget to deny at least one of 5 beacon attempts, all 5 sent")
	}
	if denied := h.budgetDeniedBroadcast(a); denied == 0 {
		t.Fatalf("budget_denied[BROADCAST] = 0, want > 0 once the lane exhausted")
	}
}

// TestBeaconBudgetGateAllowsUnderGenerousBudget is the control: with the
// default (non-shrunk) budget, node_tick's beacon path is unaffected by the
// budget gate and beacons still fire on schedule.
func TestBeaconBudgetGateAllowsUnderGenerousBudget(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addrA = 0x000000C1
	h.addNode(addrA, 0, 0)
	a := h.activateNode(addrA)
	h.forceBeaconDue(a, 0)

	n := h.tick(a, 0)
	if n != 1 {
		t.Fatalf("expected exactly 1 beacon packet on the first due tick, got %d", n)
	}
	if denied := h.budgetDeniedBroadcast(a); denied != 0 {
		t.Fatalf("budget_denied[BROADCAST] = %d, want 0 with a generous budget", denied)
	}
}
