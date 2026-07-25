package main

import (
	"encoding/json"
	"os"
	"testing"
)

// TestFloodSuppressionCancelsRedundantRelay is Flooding F1's system-level
// proof, driven through gosim's bridge.c (the REAL firmware flood path:
// _handle_data -> bridge_flood_relay -> bridge_handle_flood_relay, the same
// components the flooding transport is validated against, NOT the Go-only
// floodSim MODEL): a node that has overheard FLOOD_SUPPRESS_AFTER (2) OTHER
// copies of a flooded frame while its own rebroadcast is still waiting out its
// jitter CANCELS that now-redundant relay.
//
// Topology: a tight 5-node cluster, every node within radio range of every
// other (a "clump"). A broadcasts one channel DATA; B, C, D, E all hear it
// directly and each schedule a rebroadcast. As those relays fire in jitter
// order r1<r2<r3<r4, the node whose relay is due 3rd has, by then, overheard
// the two that already fired -> it cancels; the 4th does too. So regardless of
// the exact jitter draw, at least one relay is suppressed, emitted as a
// flood_relay_suppressed event by bridge_handle_flood_relay. collisions:false
// keeps the overhearing deterministic (the point here is the suppression
// bookkeeping, not the MAC model), mirroring flood_transport_test.go.
//
// sf:7/bw_hz:500000 (a fast PHY) is deliberate and load-bearing. Suppression
// needs FLOOD_SUPPRESS_AFTER (2) other copies fully RECEIVED before a node's
// own jitter expires: jitter_other + airtime <= jitter_self. Forward jitter is
// uniform over 50-300ms, so the widest gap between two draws is 250ms, and any
// frame longer than that cannot be overheard complete no matter how the draws
// fall. At the shipped default PHY (the frequency plan's SF9/125kHz, which
// mesh_init_radio_config programs over RADIO_PROFILE_LONG_RANGE's SF10) a
// ~32-byte frame is 263ms, and a 60-byte one 386ms, both past 250ms, so not
// one copy lands in time and suppression never triggers (the storm it prevents
// still happens). Note the bound is the 250ms SPREAD, not the 300ms top of the
// range: at 263ms the frame fits inside the range and still cannot suppress,
// which is why comparing airtime to the window alone is the wrong test. The
// fast PHY here drops a frame to ~18ms so two copies land well inside the
// spread and the mechanism is exercised; range is pinned to 150 explicitly so
// the cluster stays fully connected regardless of the SF-derived link budget.
func floodClusterScenario() string {
	return `{
		"name": "flood-suppression-cluster",
		"mode": "deterministic",
		"duration_ms": 15000,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 40,  "y": 0},
			{"id": "C", "x": 0,   "y": 40},
			{"id": "D", "x": -40, "y": 0},
			{"id": "E", "x": 0,   "y": -40}
		],
		"radio": {
			"range": 150,
			"sf": 7,
			"bw_hz": 500000,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1,
			"collisions": false
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "*"}
		]
	}`
}

func TestFloodSuppressionCancelsRedundantRelay(t *testing.T) {
	tmp, err := os.CreateTemp("", "flood-suppression-cluster-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.WriteString(floodClusterScenario()); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	tmp.Close()

	result, err := runScenarioHeadless(tmp.Name())
	if err != nil {
		t.Fatalf("runScenarioHeadless: %v", err)
	}

	suppressed := 0
	deliveredNodes := map[string]bool{}
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		switch evt["type"] {
		case "flood_relay_suppressed":
			suppressed++
		case "message_delivered":
			if node, _ := evt["node"].(string); node != "" {
				deliveredNodes[node] = true
			}
		}
	}

	// The core proof: with 4 nodes all overhearing one another, at least one
	// scheduled relay must be cancelled by overheard copies. Before this
	// change the firmware-through-bridge flood had NO suppression at all, so
	// this count was always 0 and the bridge diverged from the Go model
	// (which already suppresses, flood.go's floodSuppressAfterHeard).
	if suppressed < 1 {
		t.Fatalf("no flood relay was suppressed in a dense cluster; the firmware-through-bridge "+
			"flood is not cancelling redundant rebroadcasts (want >=1 flood_relay_suppressed, "+
			"got %d); lines:\n%s", suppressed, joinLines(result.Lines()))
	}

	// Suppression must not cost coverage: every other node still receives the
	// broadcast (the first relays to fire cover the clump before the later,
	// redundant ones cancel).
	for _, id := range []string{"B", "C", "D", "E"} {
		if !deliveredNodes[id] {
			t.Errorf("node %s never received the broadcast; suppression starved coverage", id)
		}
	}
}

// TestFloodModelAlsoSuppresses is the parity anchor: the SAME dense-cluster
// topology run under the Go-only floodSim MODEL ("routing":"flood") also
// cancels redundant relays (flood_relays_canceled >= 1). This is the behavior
// the bridge path above must reproduce; both use FLOOD_SUPPRESS_AFTER = 2, so
// the two stay qualitatively aligned (a node in a dense clump suppresses its
// relay once it has overheard two others).
func TestFloodModelAlsoSuppresses(t *testing.T) {
	// Reuse the cluster but select the model routing mode.
	scenario := `{
		"name": "flood-model-suppression-cluster",
		"mode": "deterministic",
		"duration_ms": 15000,
		"routing": "flood",
		"flood_hop_limit": 4,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 40,  "y": 0},
			{"id": "C", "x": 0,   "y": 40},
			{"id": "D", "x": -40, "y": 0},
			{"id": "E", "x": 0,   "y": -40}
		],
		"radio": {
			"range": 150,
			"sf": 7,
			"bw_hz": 500000,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1,
			"collisions": false
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "B"}
		]
	}`

	tmp, err := os.CreateTemp("", "flood-model-suppression-cluster-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.WriteString(scenario); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	tmp.Close()

	result, err := runScenarioHeadless(tmp.Name())
	if err != nil {
		t.Fatalf("runScenarioHeadless: %v", err)
	}

	var canceled float64 = -1
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if evt["type"] == "flood_final_metrics" {
			canceled, _ = evt["flood_relays_canceled"].(float64)
		}
	}
	if canceled < 0 {
		t.Fatalf("no flood_final_metrics event emitted by the model")
	}
	if canceled < 1 {
		t.Fatalf("the Go flood MODEL cancelled no relays in a dense cluster (flood_relays_canceled "+
			"= %v); the bridge path is being validated against a model that itself must suppress",
			canceled)
	}
}
