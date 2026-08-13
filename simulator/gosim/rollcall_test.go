package main

import (
	"encoding/json"
	"testing"
)

// The attested roll-call end to end over the collision-modeled simulator,
// driving the REAL components/rollcall core (codecs, canonical signed
// message, response stagger, bounded round schedule, ledger) through the real
// channel-flood relay, the real Ed25519 identity keys and pin store, and the
// real budget-gated TX path in bridge.c.
//
// Topology for both tests: a 4-node line A-B-C-D, spacing 100 units, radio
// range 150, so each node hears only its immediate neighbors and A is three
// hops from D. Every node attests its identity first, 12s apart, because a
// roll-call's expected set is exactly the anchor-certified pins the initiator
// holds and a ~230-byte attestation flooding four hops needs well over 8s of
// air to settle (the spacing TestIdentityAttestationMultiHopPinAndAddrMismatch
// established for the same reason).
//
// Both scenarios run with flood_transport enabled, and that is a statement
// about the SIMULATOR rather than about the primitive. Firmware never gates a
// roll-call answer on holding a route: mesh_rollcall.c hands the frame to
// send_data_packet, every neighbor in earshot hears it, and whichever one can
// forward it does. Gosim's radio model cannot express that under reactive
// routing, where it delivers a unicast only to the one next hop the sender
// addressed, so a reactive run measures how long the responder's route
// discovery takes to survive the roll-call's own receipt traffic rather than
// measuring the roll-call. Flood transport removes that gosim-only
// prerequisite and leaves the collision model, the airtime budget, the
// multi-hop relay and the attestation exactly as they are.
const rollcallLineTopology = `
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
		"flood_transport": true,`

// TestRollCallAttestsEveryMemberOverMultipleHops is the primitive's headline
// proof: A floods one roll-call, and all three members (one, two and three
// hops out) answer with a unicast whose Ed25519 signature verifies against
// the pin A holds for them. A's ledger closes reporting 3 of 3 expected with
// nobody missing, and every answer is attested rather than merely observed.
func TestRollCallAttestsEveryMemberOverMultipleHops(t *testing.T) {
	scenarioJSON := `{
		"name": "rollcall-4node-line",
		"mode": "deterministic",
		"duration_ms": 260000,` + rollcallLineTopology + `
		"events": [
			{"at_ms": 1000,  "type": "send_attestation", "src": "A"},
			{"at_ms": 13000, "type": "send_attestation", "src": "B"},
			{"at_ms": 25000, "type": "send_attestation", "src": "C"},
			{"at_ms": 37000, "type": "send_attestation", "src": "D"},
			{"at_ms": 60000, "type": "start_rollcall", "src": "A", "text": "sound off"}
		]
	}`

	result := writeAndRunScenario(t, "rollcall-4node-line", scenarioJSON)

	ledger, ok := result.RollCall("A")
	if !ok {
		t.Fatal("A has no roll-call ledger: the scripted start_rollcall never opened one")
	}
	if ledger.Open {
		t.Errorf("A's ledger is still open after %d ms; the close sweep never ran", 260000)
	}
	if !ledger.Anchored {
		t.Fatal("A's ledger is not anchored, so it cannot name anyone missing: bridge_init " +
			"anchors and endorses the whole fleet, so every node's pins should be " +
			"anchor-certified")
	}
	if ledger.Text != "sound off" {
		t.Errorf("ledger text = %q, want %q (the operator payload the announce carried)",
			ledger.Text, "sound off")
	}
	if ledger.Expected != 3 {
		t.Fatalf("expected set = %d, want 3 (B, C and D all attested before the roll-call, so A "+
			"holds an anchor-certified pin for each)", ledger.Expected)
	}
	if ledger.Responded != 3 {
		t.Fatalf("responded = %d of %d expected, want 3 (rows: %+v, missing: %v)",
			ledger.Responded, ledger.Expected, ledger.Rows, ledger.Missing)
	}
	if len(ledger.Missing) != 0 {
		t.Errorf("missing = %v, want empty", ledger.Missing)
	}
	if ledger.Unattested != 0 {
		t.Errorf("unattested = %d, want 0: every answer here is signed by a node A holds a "+
			"verified pin for, so none of them should fail attestation", ledger.Unattested)
	}
	if ledger.RoundsSent != 3 {
		t.Errorf("rounds_sent = %d, want %d (the schedule is bounded and always runs to its "+
			"cap; a member that already answered dedupes the later rounds)",
			ledger.RoundsSent, 3)
	}

	// Every expected member has a row, and every row is an attested answer
	// rather than a bare path observation.
	for _, id := range []string{"B", "C", "D"} {
		addr, ok := result.NodeAddr(id)
		if !ok {
			t.Fatalf("node %s not found", id)
		}
		var found *rollCallRow
		for i := range ledger.Rows {
			if ledger.Rows[i].Addr == addr {
				found = &ledger.Rows[i]
				break
			}
		}
		if found == nil {
			t.Errorf("no ledger row for %s (0x%08X)", id, addr)
			continue
		}
		if !found.Responded {
			t.Errorf("%s (0x%08X) has a row but no attested answer", id, addr)
		}
		if found.Round < 1 || found.Round > 3 {
			t.Errorf("%s answered naming round %d, want 1..3", id, found.Round)
		}
	}

	// The staggered answers are separate transmissions, not one burst: each
	// member answers exactly once no matter how many rounds it heard.
	answered := map[string]int{}
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if typ, _ := evt["type"].(string); typ == "rollcall_answered" {
			node, _ := evt["node"].(string)
			answered[node]++
		}
	}
	for _, id := range []string{"B", "C", "D"} {
		if answered[id] != 1 {
			t.Errorf("%s transmitted %d answers, want exactly 1: the answer-once table is what "+
				"keeps the re-announce rounds from costing N extra unicasts each", id,
				answered[id])
		}
	}
}

// TestRollCallReportsAPartitionedMemberMissing is the other half of the
// primitive: a member that cannot hear the roll-call is named, by address, in
// the initiator's missing set. D is pinned (it attested while still in range)
// and then moves far out of range before the roll-call starts, so A expects
// three answers, attests two, and reports D missing.
//
// What this proves is narrow on purpose, and docs/rollcall.md says so: a
// signature proves a member answered, while a MISSING entry proves only that
// no answer arrived. The mesh cannot tell this partition apart from a node
// that is switched off, out of airtime budget, or being suppressed.
func TestRollCallReportsAPartitionedMemberMissing(t *testing.T) {
	scenarioJSON := `{
		"name": "rollcall-partitioned-member",
		"mode": "deterministic",
		"duration_ms": 260000,` + rollcallLineTopology + `
		"events": [
			{"at_ms": 1000,  "type": "send_attestation", "src": "A"},
			{"at_ms": 13000, "type": "send_attestation", "src": "B"},
			{"at_ms": 25000, "type": "send_attestation", "src": "C"},
			{"at_ms": 37000, "type": "send_attestation", "src": "D"},
			{"at_ms": 50000, "type": "move_node", "node": "D", "x": 9000, "y": 9000},
			{"at_ms": 60000, "type": "start_rollcall", "src": "A", "text": "sound off"}
		]
	}`

	result := writeAndRunScenario(t, "rollcall-partitioned-member", scenarioJSON)

	ledger, ok := result.RollCall("A")
	if !ok {
		t.Fatal("A has no roll-call ledger")
	}
	if ledger.Open {
		t.Error("A's ledger is still open; the close sweep never ran")
	}
	if ledger.Expected != 3 {
		t.Fatalf("expected set = %d, want 3: D attested BEFORE it moved, so A still holds an "+
			"anchor-certified pin for it and still expects an answer", ledger.Expected)
	}
	if ledger.Responded != 2 {
		t.Fatalf("responded = %d, want 2 (B and C are still in range; D is partitioned). "+
			"rows: %+v", ledger.Responded, ledger.Rows)
	}

	addrD, ok := result.NodeAddr("D")
	if !ok {
		t.Fatal("node D not found")
	}
	if len(ledger.Missing) != 1 || ledger.Missing[0] != addrD {
		t.Fatalf("missing = %v, want exactly [0x%08X] (D)", ledger.Missing, addrD)
	}

	// D itself never answered: it heard no announce at all, so it did not
	// even queue one. A silent member is silent locally too.
	if dropped := result.RollCallPendingDropped("D"); dropped != 0 {
		t.Errorf("D dropped %d answers for a full queue, want 0: it never heard the announce, "+
			"so there was nothing to queue", dropped)
	}
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		if typ == "rollcall_answered" && node == "D" {
			t.Error("D answered while partitioned, which the topology makes impossible")
		}
	}
}
