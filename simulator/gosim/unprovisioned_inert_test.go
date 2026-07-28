package main

import (
	"encoding/json"
	"os"
	"testing"
)

// TestUnprovisionedNodeIsInert is the system-level proof for the mandatory-
// provisioning campaign (Task 2): a node that boots WITHOUT the network key is
// INERT. It originates no network-key-authenticated frame (broadcast DATA,
// identity attestation) and it accepts none (its RX is dropped at the door),
// while every provisioned node in range meshes exactly as before.
//
// Topology: three nodes A, B, C mutually in radio range. C is marked
// "unprovisioned". Scripted events:
//
//   - t=1s: A broadcasts a channel message. B (provisioned, in range) delivers
//     it; C (unprovisioned) must NOT deliver or even receive it.
//   - t=3s: C tries to broadcast a message. Inert: no frame is originated
//     (an "unprovisioned_inert" event is emitted instead), so no node ever
//     hears a DATA frame from C.
//   - t=5s: C tries to attest its identity. Inert for the same reason.
//
// Pass conditions: B delivered A's message (the fleet still works); C never
// appears as the node of a message_delivered or packet_received event (accepts
// nothing); and C emitted the inert signal for both the DATA and the
// attestation origination (emits nothing).
func TestUnprovisionedNodeIsInert(t *testing.T) {
	const scenarioJSON = `{
		"name": "mandatory-provisioning-inert-node",
		"mode": "deterministic",
		"duration_ms": 8000,
		"nodes": [
			{"id": "A", "x": 0,  "y": 0},
			{"id": "B", "x": 60, "y": 0},
			{"id": "C", "x": 0,  "y": 60, "unprovisioned": true}
		],
		"radio": {
			"range": 150,
			"loss_pct": 0,
			"propagation_speed_ms_per_unit": 0.1
		},
		"events": [
			{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "*"},
			{"at_ms": 3000, "type": "send_message", "src": "C", "dest": "*"},
			{"at_ms": 5000, "type": "send_attestation", "src": "C"}
		]
	}`

	tmp, err := os.CreateTemp("", "prov-inert-*.json")
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

	var (
		bDelivered       bool // B delivered A's broadcast: the fleet still meshes
		cTouchedTraffic  bool // C appeared in a delivered/received event: accepted something
		inertData        int  // C's DATA origination was refused
		inertAttestation int  // C's attestation origination was refused
	)

	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		switch typ {
		case "message_delivered":
			if node == "B" {
				bDelivered = true
			}
			if node == "C" {
				cTouchedTraffic = true
			}
		case "packet_received":
			if node == "C" {
				cTouchedTraffic = true
			}
		case "unprovisioned_inert":
			if node == "C" {
				switch evt["frame"] {
				case "data":
					inertData++
				case "attestation":
					inertAttestation++
				}
			}
		}
	}

	if !bDelivered {
		t.Errorf("provisioned node B never delivered A's broadcast: the mesh should keep working")
	}
	if cTouchedTraffic {
		t.Errorf("unprovisioned node C received/delivered a frame: it must accept nothing")
	}
	if inertData == 0 {
		t.Errorf("expected an unprovisioned_inert(data) event from C's blocked broadcast; got none")
	}
	if inertAttestation == 0 {
		t.Errorf("expected an unprovisioned_inert(attestation) event from C's blocked attestation; got none")
	}
}
