package main

import (
	"encoding/json"
	"testing"
)

// TestFloodModeMultiHopDeliveryAndConfirmation is the system-level proof
// that the managed-flooding routing mode (flood.go) actually relays
// across multiple hops with no route discovery, and that the
// flooded-ACK mechanism gives the STRICT (delivery-with-confirmation) bar a
// genuine, non-N/A signal.
//
// Topology: a 5-node line (A-B-C-D-E), spacing 100, radio range 150 (each
// node hears only its immediate neighbors). A sends one unicast-destined
// scripted message to E (4 hops away) under "routing":"flood",
// "flood_hop_limit":4 (enough budget for a relay at each of B, C, D).
func TestFloodModeMultiHopDeliveryAndConfirmation(t *testing.T) {
	const scenarioJSON = `{
		"name": "flood-5hop-line",
		"mode": "deterministic",
		"duration_ms": 20000,
		"routing": "flood",
		"flood_hop_limit": 4,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0},
			{"id": "C", "x": 200, "y": 0},
			{"id": "D", "x": 300, "y": 0},
			{"id": "E", "x": 400, "y": 0}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "E"}
		]
	}`

	result := writeAndRunScenario(t, "flood-5hop-line", scenarioJSON)

	var reachedE, confirmedA bool
	var finalMetrics map[string]any
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		switch evt["type"] {
		case "flood_reached":
			if evt["node"] == "E" {
				reachedE = true
			}
		case "flood_confirmed":
			if evt["node"] == "A" {
				confirmedA = true
			}
		case "flood_final_metrics":
			finalMetrics = evt
		}
	}

	if !reachedE {
		t.Fatalf("flood mode never delivered A's message to E (4 hops away) -- managed flooding "+
			"is not relaying past direct neighbors; lines:\n%s", joinLines(result.Lines()))
	}
	if !confirmedA {
		t.Fatalf("A never received a flooded ACK confirming delivery -- the STRICT " +
			"delivery-with-confirmation bar has no signal")
	}
	if finalMetrics == nil {
		t.Fatalf("no flood_final_metrics event emitted")
	}
	if reached, _ := finalMetrics["flood_reached"].(float64); reached != 1 {
		t.Errorf("flood_final_metrics.flood_reached = %v, want 1", finalMetrics["flood_reached"])
	}
	if confirmed, _ := finalMetrics["flood_confirmed"].(float64); confirmed != 1 {
		t.Errorf("flood_final_metrics.flood_confirmed = %v, want 1", finalMetrics["flood_confirmed"])
	}
}

// TestFloodModeHopLimitExpiryStopsRelay proves the hop_limit actually bounds
// propagation: with flood_hop_limit=1 on the same 5-node line, A's message
// to D (3 hops away) must NOT arrive (A -> B relays once at hop_limit=1 ->
// 0, C receives hop_limit=0 and does not relay further, D is never
// reached), while B (1 hop, direct) still is.
func TestFloodModeHopLimitExpiryStopsRelay(t *testing.T) {
	const scenarioJSON = `{
		"name": "flood-hop-limit-floor",
		"mode": "deterministic",
		"duration_ms": 20000,
		"routing": "flood",
		"flood_hop_limit": 1,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0},
			{"id": "C", "x": 200, "y": 0},
			{"id": "D", "x": 300, "y": 0}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "D"}
		]
	}`

	result := writeAndRunScenario(t, "flood-hop-limit-floor", scenarioJSON)

	var reachedD bool
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if evt["type"] == "flood_reached" && evt["node"] == "D" {
			reachedD = true
		}
	}
	if reachedD {
		t.Fatalf("D (3 hops from A) received the message with flood_hop_limit=1 -- the hop " +
			"limit is not bounding propagation")
	}
}

func joinLines(lines []string) string {
	out := ""
	for _, l := range lines {
		out += l + "\n"
	}
	return out
}
