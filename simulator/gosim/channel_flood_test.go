package main

import (
	"encoding/json"
	"os"
	"testing"
)

// TestPhase1ChannelFloodReachesFarNode is the system-level proof for Phase 1
// delivery-core plan Task 5 (bramble-meta/plans/2026-07-04-phase1-delivery-
// core-plan.md): a broadcast/channel DATA message must mesh past direct
// radio neighbors, reaching a node >=3 hops from the sender.
//
// Before this task, broadcast DATA (dest_addr == 0xFFFFFFFF) was delivered
// locally and never rebroadcast (main/mesh_task.c's handle_data), so a
// group/channel message only ever reached direct neighbors -- a core "it's
// a mesh" feature that regressed when the caller-less channel_flood module
// was deleted (703d78a1) without ever having shipped. channel_flood_decide
// (components/routing/channel_flood.c) plus its wiring into handle_data's
// broadcast branch (firmware) and this file's bridge.c broadcast branch
// (gosim, same decide function, same real airtime budget -- no parallel
// logic) restore multi-hop flooding.
//
// Topology: a 5-node line (A-B-C-D-E), spacing 100 units, radio range 150,
// so each node hears only its immediate neighbors (a 200-unit 2-hop gap
// exceeds range). A sends one broadcast/channel message; D is 3 hops away
// and E is 4 hops away. Both must receive it for the flood to have crossed
// >=3 hops.
func TestPhase1ChannelFloodReachesFarNode(t *testing.T) {
	const scenarioJSON = `{
		"name": "phase1-channel-flood-5hop-line",
		"mode": "deterministic",
		"duration_ms": 15000,
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
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "*"}
		]
	}`

	tmp, err := os.CreateTemp("", "phase1-channel-flood-5hop-line-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.WriteString(scenarioJSON); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	tmp.Close()

	result, err := runScenarioHeadless(tmp.Name())
	if err != nil {
		t.Fatalf("runScenarioHeadless: %v", err)
	}

	var packetIDHex string
	delivered := map[string]bool{}
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)

		if typ == "message_sent" && node == "A" {
			if dest, _ := evt["dest"].(string); dest == "broadcast" {
				packetIDHex, _ = evt["packet_id"].(string)
			}
		}
		if typ == "message_delivered" {
			pid, _ := evt["packet_id"].(string)
			if pid != "" && pid == packetIDHex {
				delivered[node] = true
			}
		}
	}

	if packetIDHex == "" {
		t.Fatalf("A never sent a broadcast DATA message (no message_sent/broadcast event seen)")
	}

	// Self-echo guard (final whole-branch review finding 1): once B relays
	// A's broadcast back out, A itself is in range of B and hears its own
	// message echoed back. That is not a delivery anywhere new -- A already
	// locally delivered this message the instant it originated it -- so
	// bridge.c's _handle_data broadcast branch must not emit a
	// message_delivered for A on that echo (mirrors main/mesh_task.c's
	// handle_data src_addr == s_identity->address self-guard). Before that
	// guard, A's own echo would be indistinguishable from a genuine
	// far-node delivery in this exact event stream.
	if delivered["A"] {
		t.Fatalf("originator A received a spurious message_delivered for its own broadcast %s "+
			"(self-echo from the channel flood was not suppressed)", packetIDHex)
	}

	// Direct neighbor: proves the base case still works (this passed even
	// before Task 5, since B is one hop from A).
	if !delivered["B"] {
		t.Fatalf("direct neighbor B never received broadcast %s; even single-hop broadcast is "+
			"broken", packetIDHex)
	}

	// This is the actual Task 5 proof: D (3 hops) and E (4 hops) are both
	// out of direct radio range of A (200+ units against a 150-unit range)
	// and can only receive this message if B, then C, then D each relay it
	// onward. Before Task 5, broadcast DATA was consumed locally and never
	// rebroadcast, so neither would ever see it.
	if !delivered["D"] {
		t.Fatalf("broadcast %s never reached D (3 hops from A) -- multi-hop channel flood is "+
			"not relaying past direct neighbors", packetIDHex)
	}
	if !delivered["E"] {
		t.Fatalf("broadcast %s never reached E (4 hops from A) -- multi-hop channel flood is "+
			"not relaying past direct neighbors", packetIDHex)
	}
}

// Hop-limit-floor and duplicate/budget-deny behavior are proven at the unit
// level (test/test_channel_flood.c); gosim's scenario schema and
// bridge_handle_generate_message both always originate broadcast DATA at
// ROUTE_HOP_LIMIT_MAX (mirroring firmware's send_data_packet/
// mesh_send_broadcast, which never lets a caller pick a shorter hop_limit
// for a channel message either), so there is no scenario-level knob to
// exercise a shorter budget without diverging from the shipped origination
// path.
