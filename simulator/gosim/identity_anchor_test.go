package main

import (
	"encoding/json"
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

	result := writeAndRunScenario(t, "p2-trust-anchor", scenarioJSON)

	// First pass: capture the two originators' addresses and key prefixes.
	var addrA, ed8A, addrD, ed8D string
	sawUnendorsedMark := false
	for _, line := range result.Lines() {
		var evt map[string]any
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
		var evt map[string]any
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

// TestRuntimeSetAnchorDropsStalePins is the system-level proof for the P2
// red-team fix: a fleet deployed un-anchored lets a node TOFU-pin its peers;
// when an operator later provisions the fleet anchor at runtime (bramble.
// setAnchor without a reboot, modeled by the provision_anchor event), those
// un-endorsed stale pins MUST be dropped, not survive into the anchored store.
//
// B boots un-anchored. A (endorsed) attests -> B TOFU-pins A on the self-sig
// alone (ignoring the cert). Then B is anchored at runtime: A's stale pin is
// dropped (anchor_provisioned reports dropped_pins=1). A attests again -> B, now
// anchored, verifies A's endorsement and re-pins A as a fresh NEW binding. So B
// emits identity_pinned for A exactly TWICE: the drop is what makes the second
// attestation a NEW pin instead of a silent idempotent refresh. Without the fix
// the stale pin would survive and the re-attestation would be a REFRESH (one
// pin event, and an un-endorsed binding left pinned on an anchored node).
func TestRuntimeSetAnchorDropsStalePins(t *testing.T) {
	const scenarioJSON = `{
		"name": "p2-runtime-setanchor",
		"mode": "deterministic",
		"duration_ms": 30000,
		"nodes": [
			{"id": "A", "x": 0,   "y": 0},
			{"id": "B", "x": 100, "y": 0, "unanchored": true}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000,  "type": "send_attestation", "src": "A"},
			{"at_ms": 13000, "type": "provision_anchor", "node": "B"},
			{"at_ms": 20000, "type": "send_attestation", "src": "A"}
		]
	}`

	result := writeAndRunScenario(t, "p2-runtime-setanchor", scenarioJSON)

	var addrA string
	sawUnanchored := false
	pinnedACount := 0
	droppedPins := -1
	provisionTsUs := uint64(0)
	var lastPinTsUs uint64
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		switch typ {
		case "node_unanchored":
			if node == "B" {
				sawUnanchored = true
			}
		case "attestation_sent":
			if node == "A" && addrA == "" {
				addrA, _ = evt["addr"].(string)
			}
		case "anchor_provisioned":
			if node == "B" {
				if v, ok := evt["dropped_pins"].(float64); ok {
					droppedPins = int(v)
				}
				if ts, ok := evt["timestamp_us"].(float64); ok {
					provisionTsUs = uint64(ts)
				}
			}
		}
	}
	if !sawUnanchored {
		t.Fatalf("B was never marked unanchored: the scenario override did not apply")
	}
	if addrA == "" {
		t.Fatalf("A never emitted attestation_sent")
	}

	// Count B's pins of A, and note the last one's timestamp.
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if typ, _ := evt["type"].(string); typ != "identity_pinned" {
			continue
		}
		if node, _ := evt["node"].(string); node != "B" {
			continue
		}
		if addr, _ := evt["addr"].(string); addr != addrA {
			continue
		}
		pinnedACount++
		if ts, ok := evt["timestamp_us"].(float64); ok {
			lastPinTsUs = uint64(ts)
		}
	}

	// The fix: provisioning dropped exactly the one stale TOFU pin.
	if droppedPins != 1 {
		t.Fatalf("anchor_provisioned dropped_pins = %d, want 1 (the stale TOFU pin of A must be "+
			"dropped when B is anchored at runtime)", droppedPins)
	}
	// The observable consequence: B re-pins A as a fresh NEW binding AFTER the
	// drop, so there are two pin events, and the second is post-provision. A
	// surviving pin would have made the re-attestation a silent refresh (one
	// event only).
	if pinnedACount != 2 {
		t.Fatalf("B pinned A %d time(s), want 2 (TOFU pin, then a fresh NEW re-pin after the "+
			"stale pin was dropped at provision); one pin means the stale pin wrongly survived",
			pinnedACount)
	}
	if lastPinTsUs <= provisionTsUs {
		t.Fatalf("B's last pin of A (t=%d us) was not after provision (t=%d us): the endorsed "+
			"re-pin must follow the anchor provisioning", lastPinTsUs, provisionTsUs)
	}
}
