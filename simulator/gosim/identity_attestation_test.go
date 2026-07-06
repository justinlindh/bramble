package main

import (
	"encoding/json"
	"os"
	"testing"
)

// TestIdentityAttestationMultiHopPinAndAddrMismatch is the system-level
// proof for per-node identity Phases 3+4: identity attestations flood
// through the REAL firmware relay path (relay-gate MAC, shared
// channel-flood engine) and every receiver verifies + TOFU-pins; an
// insider claiming someone else's address is refused by every receiver's
// addr<->key check (Phase 4), pin or no pin.
//
// Topology: a 5-node line (A-B-C-D-E), spacing 100 units, radio range 150,
// so each node hears only its immediate neighbors. Two scripted events:
//
//   - t=1s: A attests its own identity. The frame must cross B, C, D to
//     reach E (4 hops): every one of them relays it (MAC-gated, hop_limit
//     decremented, otherwise unmodified) AND pins A's binding.
//   - t=13s (after the first flood has fully settled: a 230-byte frame
//     costs ~2.1s of airtime at the sim datarate, so a 4-hop flood needs
//     well over 8s, and an earlier TX from E half-duplex-collides with D's
//     relay of the genuine attestation):
//     E (a keyed insider: it holds the network key, so its frame relays
//     fine) attests A's ADDRESS under E's OWN Ed25519 key. The frame is
//     internally valid (its sig verifies against its embedded key), but
//     A's address does not derive from E's key: since the Phase 4 rebind
//     EVERY receiver rejects it at the addr<->key check
//     (identity_addr_mismatch), even one that never heard the genuine A.
//     Pre-Phase-4 this was only caught by the TOFU pin (first-seen wins);
//     now it is cryptographically infeasible to claim at all.
func TestIdentityAttestationMultiHopPinAndAddrMismatch(t *testing.T) {
	const scenarioJSON = `{
		"name": "phase4-identity-attestation-line",
		"mode": "deterministic",
		"duration_ms": 30000,
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
			{"at_ms": 13000, "type": "send_attestation", "src": "E", "claim": "A"}
		]
	}`

	tmp, err := os.CreateTemp("", "phase4-identity-attestation-*.json")
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
	pinned := map[string]string{}   // node -> ed8 pinned for A's address
	mismatches := map[string]bool{} // node -> saw identity_addr_mismatch for A's address
	conflicts := map[string]bool{}  // node -> saw identity_conflict for A's address

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
		case "identity_addr_mismatch":
			ed8, _ := evt["ed8"].(string)
			if ed8 != ed8E {
				t.Fatalf("%s rejected addr-mismatched key %s, expected the attacker's %s",
					node, ed8, ed8E)
			}
			mismatches[node] = true
		case "identity_conflict":
			conflicts[node] = true
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

	// Self-guard: A must never pin (or reject) its own address.
	if _, ok := pinned["A"]; ok {
		t.Fatalf("A pinned its own address %s: self-attestations must be ignored", addrA)
	}

	// The Phase 4 payoff: the forged claim is refused at the addr<->key
	// check by every receiver. D is adjacent to the attacker and must see
	// it; C proves the forged frame also RELAYED (MAC-valid keyed-insider
	// traffic does flood; rejection is at delivery, not the relay).
	for _, n := range []string{"C", "D"} {
		if !mismatches[n] {
			t.Fatalf("%s never emitted identity_addr_mismatch: the impersonation of %s was "+
				"not refused at the addr<->key check", n, addrA)
		}
	}
	// And it never even reaches the TOFU-conflict stage: the addr check
	// fires first, so no receiver reports a conflict for A's address.
	for n := range conflicts {
		t.Fatalf("%s emitted identity_conflict for %s: the forged claim should have been "+
			"rejected at the addr<->key check before any pin comparison", n, addrA)
	}
}

// TestIdentityAttestationX25519RotationConflict pins what the address does
// NOT bind: the X25519 (DM) key. A validly signed re-attestation from the
// SAME node (same Ed key, so the Phase 4 addr<->key check passes) carrying
// a DIFFERENT X25519 pub must be refused by receivers holding the original
// pin, as a TOFU CONFLICT: this is the DM-key-continuity red flag the
// Phase 4 DM gate consumes, and post-rebind it is the only remaining road
// to a conflict (short of a 2^32-work address-colliding Ed key).
func TestIdentityAttestationX25519RotationConflict(t *testing.T) {
	const scenarioJSON = `{
		"name": "phase4-identity-x25519-rotation",
		"mode": "deterministic",
		"duration_ms": 20000,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0},
			{"id": "C", "x": 200, "y": 0}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000, "type": "send_attestation", "src": "A"},
			{"at_ms": 9000, "type": "send_attestation", "src": "A", "rotate_x25519": true}
		]
	}`

	tmp, err := os.CreateTemp("", "phase4-identity-rotation-*.json")
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

	var addrA, ed8A string
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if typ, _ := evt["type"].(string); typ == "attestation_sent" {
			if node, _ := evt["node"].(string); node == "A" && addrA == "" {
				addrA, _ = evt["addr"].(string)
				ed8A, _ = evt["ed8"].(string)
			}
		}
	}
	if addrA == "" {
		t.Fatalf("A never emitted attestation_sent")
	}

	pinned := map[string]bool{}
	conflicts := map[string]bool{}
	mismatches := map[string]bool{}
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
			pinned[node] = true
		case "identity_conflict":
			// Same Ed key on both sides proves the conflict is about the
			// rotated X25519 key, not the signing identity.
			kept, _ := evt["kept_ed8"].(string)
			rejected, _ := evt["rejected_ed8"].(string)
			if kept != ed8A || rejected != ed8A {
				t.Fatalf("%s conflict kept=%s rejected=%s, expected both to be A's Ed key %s "+
					"(an X25519-only rotation)", node, kept, rejected, ed8A)
			}
			conflicts[node] = true
		case "identity_addr_mismatch":
			mismatches[node] = true
		}
	}

	for _, n := range []string{"B", "C"} {
		if !pinned[n] {
			t.Fatalf("%s never pinned A's genuine binding (control failed)", n)
		}
		if !conflicts[n] {
			t.Fatalf("%s never emitted identity_conflict for A's X25519 rotation: the "+
				"DM-key-change red flag went undetected", n)
		}
		if mismatches[n] {
			t.Fatalf("%s emitted identity_addr_mismatch for A's rotation: same Ed key, the "+
				"addr<->key check must PASS here", n)
		}
	}
}

// TestSimNodeAddressesDeriveFromIdentityKeys pins the Phase 4 rebind
// invariant inside the sim itself: every scenario-loaded node's address
// equals SHA256(its Ed25519 identity pub)[0:4], computed independently in
// Go. This is what entitles gosim attestations to pass the REAL firmware
// addr<->key check in identity_store_handle_attestation.
func TestSimNodeAddressesDeriveFromIdentityKeys(t *testing.T) {
	const scenarioJSON = `{
		"name": "phase4-addr-derivation",
		"mode": "deterministic",
		"duration_ms": 1000,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0},
			{"id": "C", "x": 200, "y": 0}
		],
		"radio": {"range": 150, "loss_pct": 0}
	}`

	tmp, err := os.CreateTemp("", "phase4-addr-derivation-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.WriteString(scenarioJSON); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	tmp.Close()

	h := newRadioHarness()
	defer h.free()
	if _, ok := loadScenario(tmp.Name(), h.nodes, h.radio, h.events, h.rng); !ok {
		t.Fatalf("loadScenario failed")
	}
	if h.nodeCount() != 3 {
		t.Fatalf("nodeCount = %d, want 3", h.nodeCount())
	}
	seen := map[uint32]bool{}
	for i := 0; i < h.nodeCount(); i++ {
		addr, derived := h.nodeAddrAndDerived(i)
		if addr != derived {
			t.Fatalf("node %d: addr 0x%08X != derive(ed_pub) 0x%08X (Phase 4 rebind invariant)",
				i, addr, derived)
		}
		if seen[addr] {
			t.Fatalf("duplicate derived address 0x%08X", addr)
		}
		seen[addr] = true
	}
}
