package main

import (
	"encoding/json"
	"os"
	"testing"
)

// TestIdentityAnchorEndorsedPinsUnendorsedRejected is the system-level proof
// for the trust-anchor pin gate (P2): under the REAL firmware pin store, an
// ANCHORED node pins ONLY anchor-endorsed identities. gosim anchors every node
// to a fixed test anchor and endorses the whole fleet by default (bridge.c),
// so an all-endorsed mesh pins normally through the flood; a node marked
// "unendorsed" attests a MAC-valid, relayed frame that carries no fleet-anchor
// cert, and NO anchored receiver pins it (each emits identity_unendorsed
// instead), even the ones many hops away that the frame floods to.
//
// Topology: a 4-node line A-B-C-D (spacing 100, range 150, neighbors only).
// D is marked unendorsed. Scripted events:
//
//   - t=1s: A attests (endorsed). The frame crosses B and C to reach D
//     (3 hops): every anchored receiver verifies the endorsement AND pins A.
//   - t=13s (after the first flood settles: a 230-byte frame is ~2.1s/hop):
//     D attests. Its frame is network-key-MAC-valid so it relays across C, B
//     to A, but it carries not_after=0 (no cert), so C, B and A each REFUSE to
//     pin it (identity_unendorsed) while still relaying it. D's address is
//     pinned by nobody.
func TestIdentityAnchorEndorsedPinsUnendorsedRejected(t *testing.T) {
	const scenarioJSON = `{
		"name": "p2-trust-anchor-line",
		"mode": "deterministic",
		"duration_ms": 30000,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0},
			{"id": "C", "x": 200, "y": 0},
			{"id": "D", "x": 300, "y": 0, "unendorsed": true}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000,  "type": "send_attestation", "src": "A"},
			{"at_ms": 13000, "type": "send_attestation", "src": "D"}
		]
	}`

	tmp, err := os.CreateTemp("", "p2-trust-anchor-*.json")
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

	// First pass: capture the two originators' addresses and key prefixes.
	var addrA, ed8A, addrD, ed8D string
	sawUnendorsedMark := false
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		switch evt["type"].(string) {
		case "node_unendorsed":
			if node, _ := evt["node"].(string); node == "D" {
				sawUnendorsedMark = true
			}
		case "attestation_sent":
			node, _ := evt["node"].(string)
			if node == "A" {
				addrA, _ = evt["addr"].(string)
				ed8A, _ = evt["ed8"].(string)
			}
			if node == "D" {
				addrD, _ = evt["addr"].(string)
				ed8D, _ = evt["ed8"].(string)
			}
		}
	}
	if !sawUnendorsedMark {
		t.Fatalf("D was never marked unendorsed: the scenario override did not apply")
	}
	if addrA == "" || ed8A == "" {
		t.Fatalf("A never emitted attestation_sent (endorsed origination missing)")
	}
	// D must still ORIGINATE and RELAY (it holds the network key): unendorsed
	// gates pinning, not liveness. A missing attestation_sent would make the
	// rejection assertion vacuous.
	if addrD == "" || ed8D == "" {
		t.Fatalf("D never emitted attestation_sent: unendorsed must not block origination")
	}
	if addrA == addrD {
		t.Fatalf("test is vacuous: A and D share an address (%s)", addrA)
	}

	// Second pass: pins and unendorsed rejections, keyed by attested address.
	pinnedA := map[string]bool{}     // node -> pinned A's endorsed binding
	pinnedD := map[string]bool{}     // node -> (wrongly) pinned D's address
	unendorsedD := map[string]bool{} // node -> refused D as unendorsed
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		addr, _ := evt["addr"].(string)
		switch typ {
		case "identity_pinned":
			if addr == addrA {
				pinnedA[node] = true
			}
			if addr == addrD {
				pinnedD[node] = true
			}
		case "identity_unendorsed":
			if addr == addrD {
				ed8, _ := evt["ed8"].(string)
				if ed8 != ed8D {
					t.Fatalf("%s refused unendorsed key %s for %s, expected D's %s",
						node, ed8, addrD, ed8D)
				}
				unendorsedD[node] = true
			}
			if addr == addrA {
				t.Fatalf("%s refused A as unendorsed: A is endorsed and must pin", node)
			}
		}
	}

	// Control: the all-endorsed flood pins normally. B, C, D are 1..3 hops from
	// A; every one pinning A proves the endorsed cert verified at each hop under
	// the endorsed-only gate.
	for _, n := range []string{"B", "C", "D"} {
		if !pinnedA[n] {
			t.Fatalf("%s never pinned endorsed originator A (%s): the endorsed-only gate "+
				"wrongly rejected a valid cert, or the flood did not arrive", n, addrA)
		}
	}

	// The P2 payoff: D's unendorsed attestation is pinned by NOBODY.
	for n := range pinnedD {
		t.Fatalf("%s pinned unendorsed node D (%s): an anchored node must pin only "+
			"endorsed identities", n, addrD)
	}

	// And it is actively refused as unendorsed by the receivers it floods to.
	// C is adjacent to D; B is one relay hop further, proving the MAC-valid
	// unendorsed frame still RELAYED (rejection is at pinning, not the relay).
	for _, n := range []string{"C", "B"} {
		if !unendorsedD[n] {
			t.Fatalf("%s never emitted identity_unendorsed for D (%s): the unendorsed "+
				"attestation was not refused at the pin gate (or did not reach %s)", n, addrD, n)
		}
	}

	// Self-guard: D must never pin its own address.
	if pinnedD["D"] {
		t.Fatalf("D pinned its own address %s: self-attestations must be ignored", addrD)
	}
}
