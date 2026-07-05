package main

import (
	"encoding/json"
	"os"
	"testing"
)

// TestIdentityAttestationMultiHopPinAndConflict is the system-level proof
// for per-node identity Phase 3: identity attestations flood through the
// REAL firmware relay path (relay-gate MAC, shared channel-flood engine)
// and every receiver verifies + TOFU-pins, with impersonation-conflict
// detection at the far edge.
//
// Topology: a 5-node line (A-B-C-D-E), spacing 100 units, radio range 150,
// so each node hears only its immediate neighbors. Two scripted events:
//
//   - t=1s: A attests its own identity. The frame must cross B, C, D to
//     reach E (4 hops): every one of them relays it (MAC-gated, hop_limit
//     decremented, otherwise unmodified) AND pins A's binding.
//   - t=9s (after the first flood has fully settled: a 158-byte frame
//     costs ~1.5s of airtime at the sim datarate, and an earlier TX from E
//     half-duplex-collides with D's relay of the genuine attestation):
//     E (a keyed insider: it holds the network key, so its frame
//     relays fine) attests A's ADDRESS under E's OWN Ed25519 key. The
//     frame is internally valid (its sig verifies against its embedded
//     key), so only the TOFU pin can catch it: every node that already
//     pinned the genuine A must emit identity_conflict, KEEP A's original
//     key, and never pin the forged binding.
func TestIdentityAttestationMultiHopPinAndConflict(t *testing.T) {
	const scenarioJSON = `{
		"name": "phase3-identity-attestation-line",
		"mode": "deterministic",
		"duration_ms": 22000,
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
			{"at_ms": 1000, "type": "send_attestation", "src": "A"},
			{"at_ms": 9000, "type": "send_attestation", "src": "E", "claim": "A"}
		]
	}`

	tmp, err := os.CreateTemp("", "phase3-identity-attestation-*.json")
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

	var addrA, ed8A, ed8E string
	pinned := map[string]string{}  // node -> ed8 pinned for A's address
	conflicts := map[string]bool{} // node -> saw identity_conflict for A's address
	keptWrong := map[string]string{}

	// First pass: capture the two attestation_sent events (A's genuine one
	// and E's forged claim of A's address).
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if typ, _ := evt["type"].(string); typ == "attestation_sent" {
			node, _ := evt["node"].(string)
			if node == "A" {
				addrA, _ = evt["addr"].(string)
				ed8A, _ = evt["ed8"].(string)
			}
			if node == "E" {
				ed8E, _ = evt["ed8"].(string)
			}
		}
	}
	if addrA == "" || ed8A == "" {
		t.Fatalf("A never emitted attestation_sent (genuine origination missing)")
	}
	if ed8E == "" {
		t.Fatalf("E never emitted attestation_sent (forged origination missing)")
	}
	if ed8E == ed8A {
		t.Fatalf("test is vacuous: attacker E's key prefix equals A's (%s)", ed8A)
	}

	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		addr, _ := evt["addr"].(string)
		if addr != addrA {
			continue
		}
		switch typ {
		case "identity_pinned":
			ed8, _ := evt["ed8"].(string)
			if prev, ok := pinned[node]; ok {
				t.Fatalf("%s pinned %s twice (%s then %s): re-pin must be an idempotent "+
					"refresh, not a new pin event", node, addrA, prev, ed8)
			}
			pinned[node] = ed8
		case "identity_conflict":
			conflicts[node] = true
			kept, _ := evt["kept_ed8"].(string)
			rejected, _ := evt["rejected_ed8"].(string)
			if kept != ed8A {
				keptWrong[node] = kept
			}
			if rejected != ed8E {
				t.Fatalf("%s rejected key %s in the conflict, expected the attacker's %s",
					node, rejected, ed8E)
			}
		}
	}

	// Multi-hop pin: E is 4 hops from A (400 units against a 150-unit
	// range), so E pinning A proves B, C and D each MAC-verified and
	// relayed the attestation through the shared flood engine.
	for _, n := range []string{"B", "C", "D", "E"} {
		ed8, ok := pinned[n]
		if !ok {
			t.Fatalf("%s never pinned A's identity %s: attestation did not reach or verify",
				n, addrA)
		}
		if ed8 != ed8A {
			t.Fatalf("%s pinned key %s for A, expected A's genuine %s", n, ed8, ed8A)
		}
	}

	// Self-guard: A must never pin (or conflict on) its own address.
	if _, ok := pinned["A"]; ok {
		t.Fatalf("A pinned its own address %s: self-attestations must be ignored", addrA)
	}
	if conflicts["A"] {
		t.Fatalf("A emitted identity_conflict for its own address: the forged frame must be "+
			"ignored as self, not treated as a binding for %s", addrA)
	}

	// The impersonation payoff: nodes that pinned the genuine A detect the
	// forged claim and KEEP A's original key. D is adjacent to the
	// attacker and must see it; C proves the forged frame also RELAYED
	// (MAC-valid keyed-insider traffic does flood; detection is at the
	// pin, not the relay).
	for _, n := range []string{"C", "D"} {
		if !conflicts[n] {
			t.Fatalf("%s never emitted identity_conflict: the impersonation of %s went "+
				"undetected", n, addrA)
		}
		if kept, bad := keptWrong[n]; bad {
			t.Fatalf("%s kept key %s after the conflict, expected A's original %s "+
				"(first-seen must win)", n, kept, ed8A)
		}
	}
}
