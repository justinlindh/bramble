package main

import (
	"encoding/json"
	"testing"
)

// intermediateRREPStarScenario is a star topology (I at the hub, D/E/F each
// a spoke in range of I only, not of each other): I=(0,0), D=(140,0),
// E=(-140,0), F=(0,140). At the sim's default SF10/125kHz derived range
// (~149.9 units, see the SF-range coupling fix, commit 30603bdc), I hears
// all three spokes but no spoke hears another (E-D is 280 units, F-D is
// ~198, E-F is ~313, all out of range). "collisions":false keeps this
// deterministic: the point of this test is the routing decision, not the
// MAC/collision model.
//
// E sends to D first (t=1s): I has no cached route to D yet, so it forwards
// the RREQ normally (classic flood) and D itself replies. That reply gives I
// a fresh ROUTE_SRC_DISCOVERED route to D.
//
// F then sends to D (t=5s), well inside INTERMEDIATE_RREP_MAX_AGE_MS: with
// intermediate-node RREP enabled, I now answers F's RREQ directly on D's
// behalf instead of forwarding it, so D never even sees this second
// discovery. Disabled, I forwards it and D answers again, same as any
// ordinary flood.
func intermediateRREPStarScenario(intermediateRREP bool) string {
	type scenario struct {
		Name             string `json:"name"`
		Mode             string `json:"mode"`
		DurationMs       int    `json:"duration_ms"`
		IntermediateRREP *bool  `json:"intermediate_rrep,omitempty"`
		Nodes            []any  `json:"nodes"`
		Radio            any    `json:"radio"`
		Events           []any  `json:"events"`
	}
	node := func(id string, x, y float64) map[string]any {
		return map[string]any{"id": id, "x": x, "y": y}
	}
	sendEvt := func(atMs int, src, dest string) map[string]any {
		return map[string]any{"at_ms": atMs, "type": "send_message", "src": src, "dest": dest}
	}
	s := scenario{
		Name:       "intermediate-rrep-star",
		Mode:       "deterministic",
		DurationMs: 30000,
		Nodes: []any{
			node("I", 0, 0),
			node("D", 140, 0),
			node("E", -140, 0),
			node("F", 0, 140),
		},
		Radio: map[string]any{
			"loss_pct":                      0,
			"propagation_speed_ms_per_unit": 0.1,
			"collisions":                    false,
		},
		Events: []any{
			sendEvt(1000, "E", "D"),
			sendEvt(5000, "F", "D"),
		},
	}
	// Only set the field when explicitly overriding away from the default
	// (true), so the "enabled" run also proves the field-omitted default
	// path, not just the explicit-true path.
	if !intermediateRREP {
		v := false
		s.IntermediateRREP = &v
	}
	b, err := json.Marshal(s)
	if err != nil {
		panic(err)
	}
	return string(b)
}

// TestIntermediateRREPShortCircuitsSecondDiscovery is Phase 2 "save
// reactive routing" Part B's system-level proof, enabled (the default):
// once I has a route to D, F's later RREQ for D is answered directly by I
// instead of flooding all the way to D. Both messages still get delivered
// AND confirmed (correctness preserved); the RREQ count drops relative to
// the disabled case (see TestIntermediateRREPDisabledFloodsFully), the
// mechanism's actual airtime-saving proof.
func TestIntermediateRREPShortCircuitsSecondDiscovery(t *testing.T) {
	finalMetrics := runAndGetFinalMetrics(t, "intermediate-rrep-star-on",
		intermediateRREPStarScenario(true))

	delivered, _ := finalMetrics["delivered"].(float64)
	confirmed, _ := finalMetrics["confirmed"].(float64)
	rreqsSent, _ := finalMetrics["rreqs_sent"].(float64)

	if delivered != 2 {
		t.Fatalf("delivered = %v, want 2 (both E->D and F->D should reach D)", finalMetrics["delivered"])
	}
	if confirmed != 2 {
		t.Fatalf("confirmed = %v, want 2 (both delivery receipts should return)", finalMetrics["confirmed"])
	}
	// E's discovery (I has no cached route yet) floods normally: E's RREQ +
	// I's forward = 2. F's discovery is short-circuited by I's intermediate
	// reply: F's RREQ only = 1 (no forward on to D). Total 3. Some extra
	// RREQ retries can occur at these tight timings, so assert an upper
	// bound that would fail if the short-circuit stopped happening (8, the
	// disabled case's count) rather than pin an exact fragile number.
	if rreqsSent >= 8 {
		t.Errorf("rreqs_sent = %v, want < 8: intermediate RREP should have short-circuited F's "+
			"discovery at I instead of flooding it all the way to D like the disabled case does",
			rreqsSent)
	}
}

// TestIntermediateRREPDisabledFloodsFully is the A/B baseline: with
// "intermediate_rrep":false on the identical star topology/traffic, I must
// NOT answer on D's behalf even though it holds the exact same fresh cached
// route; F's RREQ floods all the way to D and back, same as any ordinary
// discovery. This is both the toggle's own correctness proof (it actually
// disables the feature) and the "before" measurement baseline.
func TestIntermediateRREPDisabledFloodsFully(t *testing.T) {
	finalMetrics := runAndGetFinalMetrics(t, "intermediate-rrep-star-off",
		intermediateRREPStarScenario(false))

	delivered, _ := finalMetrics["delivered"].(float64)
	confirmed, _ := finalMetrics["confirmed"].(float64)

	if delivered != 2 {
		t.Fatalf("delivered = %v, want 2", finalMetrics["delivered"])
	}
	if confirmed != 2 {
		t.Fatalf("confirmed = %v, want 2", finalMetrics["confirmed"])
	}
	// No short-circuit: this scenario's own full-flood RREQ count. Compared
	// against TestIntermediateRREPShortCircuitsSecondDiscovery's tighter
	// upper bound, this is what proves the feature does something (rather
	// than the enabled run simply never having anything to short-circuit).
	rreqsSent, _ := finalMetrics["rreqs_sent"].(float64)
	if rreqsSent < 8 {
		t.Errorf("rreqs_sent = %v, want >= 8 (disabled: F's discovery should flood all the way to "+
			"D, same as E's did)", rreqsSent)
	}
}
