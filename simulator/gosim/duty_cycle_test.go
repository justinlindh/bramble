package main

import (
	"os"
	"testing"
)

// TestDutyCycleCapForcesBeaconThrottling proves Task 5's wiring: applying a
// tight regulatory duty-cycle cap (1%, EU868-style) via the REAL
// airtime_budget_set_duty_cap forces node_tick's beacon budget gate (Task 1)
// to start denying beacons, something the default (unlimited) profile would
// not do at the same cadence. No sim-side duty math is involved: the cap is
// applied once, and the existing budget gate does the rest.
func TestDutyCycleCapForcesBeaconThrottling(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addr = 0x000000FA
	h.addNode(addr, 0, 0)
	n := h.activateNode(addr)
	h.applyDutyCap(n, 1) // 1% duty cycle: tight enough that beacons cannot sustain 60s cadence
	h.forceBeaconDue(n, 0)

	now := uint64(0)
	denied := false
	for i := 0; i < 20 && !denied; i++ {
		h.tick(n, now)
		if h.budgetDeniedBroadcast(n) > 0 {
			denied = true
		}
		next := h.nextBeaconDue(n)
		if next <= now {
			next = now + 70_000_000 // fallback: force progress if unchanged
		}
		now = next
	}

	if !denied {
		t.Fatalf("expected budget_denied[BROADCAST] > 0 within 20 beacon attempts under a 1%% " +
			"duty cap, got 0: the cap should have throttled beacons well before this many attempts")
	}
}

// TestDutyCycleCapAbsentAllowsNormalBeaconing is the control: with no duty
// cap applied (today's default, "radio.duty_cycle_pct" absent from a
// scenario), the same beacon cadence must NOT be throttled, proving the
// denials above come from the cap, not from some other budget quirk.
func TestDutyCycleCapAbsentAllowsNormalBeaconing(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addr = 0x000000FB
	h.addNode(addr, 0, 0)
	n := h.activateNode(addr)
	// No applyDutyCap call: default unenforced, unlimited profile.
	h.forceBeaconDue(n, 0)

	now := uint64(0)
	for i := 0; i < 20; i++ {
		h.tick(n, now)
		next := h.nextBeaconDue(n)
		if next <= now {
			next = now + 70_000_000
		}
		now = next
	}

	if denied := h.budgetDeniedBroadcast(n); denied != 0 {
		t.Fatalf("budget_denied[BROADCAST] = %d, want 0 without a duty cap (default unlimited)",
			denied)
	}
}

// TestDutyCycleCapScenarioSchemaAppliesThroughBridge exercises the actual
// new plumbing end to end: a scenario JSON's "radio.duty_cycle_pct" field
// parses into the shared radio_config_t (sim_scenario.c), and
// bridge_apply_duty_cycle_cap (the exact function sim.go calls after every
// node_activate) applies it to a node's real airtime budget, which then
// throttles beacons via Task 1's existing gate. This is the path
// TestDutyCycleCapForcesBeaconThrottling does NOT cover (that test calls
// airtime_budget_set_duty_cap directly, skipping the schema and bridge glue).
func TestDutyCycleCapScenarioSchemaAppliesThroughBridge(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	tmp, err := os.CreateTemp("", "duty-cycle-scenario-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())
	const scenarioJSON = `{
		"name": "duty-cycle-smoke",
		"duration_ms": 1000,
		"seed": 1,
		"radio": {"duty_cycle_pct": 1},
		"nodes": [{"id": "A", "x": 0, "y": 0}]
	}`
	if _, err := tmp.WriteString(scenarioJSON); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	tmp.Close()

	if _, ok := loadScenario(tmp.Name(), h.nodes, h.radio, h.events, h.rng); !ok {
		t.Fatalf("loadScenario failed for a valid duty_cycle_pct scenario")
	}
	if !h.dutyCycleSet() {
		t.Fatalf("radio.duty_cycle_set = false after loading a scenario with duty_cycle_pct")
	}
	if got := h.dutyCycleCapPct(); got != 1 {
		t.Fatalf("radio.duty_cycle_pct = %d, want 1", got)
	}

	// Since the Phase 4 rebind, scenario-loaded nodes get addresses derived
	// from their Ed25519 identity keys (deterministic per node id, not a
	// fixed constant); this scenario has exactly one node, so take it by
	// index and activate it the way cmdLoad does.
	n := h.nodeAtIndex(0)
	nodeActivate(n)
	h.applyBridgeDutyCycleCap(n) // exactly what cmdLoad does after nodeActivate
	h.forceBeaconDue(n, 0)

	now := uint64(0)
	denied := false
	for i := 0; i < 20 && !denied; i++ {
		h.tick(n, now)
		if h.budgetDeniedBroadcast(n) > 0 {
			denied = true
		}
		next := h.nextBeaconDue(n)
		if next <= now {
			next = now + 70_000_000
		}
		now = next
	}

	if !denied {
		t.Fatalf("expected budget_denied[BROADCAST] > 0 via the scenario-loaded duty cap, got 0")
	}
}
