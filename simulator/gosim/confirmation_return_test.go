package main

import (
	"encoding/json"
	"os"
	"strconv"
	"strings"
	"testing"
)

// TestPhase1ConfirmationNeverReachesOriginatorAcrossMultiHopLine is the
// system-level TDD baseline for Phase 1 delivery-core plan Task 1
// (bramble-meta/plans/2026-07-04-phase1-delivery-core-plan.md).
//
// gosim used to install a route to every beacon sender (the old
// _handle_beacon in bridge.c), which firmware's own handle_beacon
// (main/mesh_task.c) never does. That accidentally supplied the reverse-hop
// route real relays never get, which masked the confirmation-return bug:
// relays only ever install routes TOWARD an RREQ/RREP discovery target
// (rrep_rx_decide), never back toward a message's originator. With that
// masking removed, this scenario proves gosim now reproduces the real
// failure: on a line topology forcing 3 hops (A-B-C-D, each hop just barely
// in range, non-adjacent pairs out of range), A's unicast DATA to D is
// delivered (D decodes it), but D's delivery receipt can never route back
// to A (no node on the path, including D itself, ever learned a route
// toward A), so A never observes a confirmation.
func TestPhase1ConfirmationNeverReachesOriginatorAcrossMultiHopLine(t *testing.T) {
	const scenarioJSON = `{
		"name": "phase1-line-4hop",
		"mode": "deterministic",
		"duration_ms": 15000,
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

	tmp, err := os.CreateTemp("", "phase1-line-4hop-*.json")
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
	destDelivered := false
	sourceConfirmed := false
	for _, line := range result.Lines() {
		if !strings.Contains(line, `"type":"message_`) {
			continue
		}
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		pid, _ := evt["packet_id"].(string)

		if typ == "message_sent" && node == "A" {
			packetIDHex = pid
		}
		if typ == "message_delivered" && pid != "" && pid == packetIDHex {
			switch node {
			case "D":
				destDelivered = true
			case "A":
				sourceConfirmed = true
			}
		}
	}

	if packetIDHex == "" {
		t.Fatalf("A never sent a DATA message to D (no message_sent event seen); " +
			"route discovery across the 3-hop line did not complete within the scenario window")
	}
	if !destDelivered {
		t.Fatalf("destination D never decoded packet %s even though a route existed via "+
			"RREQ/RREP discovery; multi-hop DATA forwarding is broken", packetIDHex)
	}

	/* BUG (phase1): confirmation return path broken, relays have no route
	 * back to the originator; Task 4 flips this assertion. */
	if sourceConfirmed {
		t.Fatalf("source A observed a delivery confirmation for %s. Per the Phase 1 "+
			"delivery-core plan this should currently be impossible: relays only ever "+
			"install routes TOWARD a discovery target (rrep_rx_decide), never back toward "+
			"the originator, so D's delivery receipt has no route home. If this now "+
			"passes, Task 4 (wire v4 prev_hop reverse-route learning) has landed and this "+
			"assertion should be flipped to require sourceConfirmed", packetIDHex)
	}

	// The scenario's 15s duration is chosen to land well before the
	// NORMAL-tier pending-ack retry backoff exhausts (base 2s, doubling,
	// max 3 attempts: exhaustion lands around t=~20s after send at t=1s),
	// so a still-active pending ack here reflects "no receipt has arrived",
	// not "gave up after retries" (which would also clear it, for an
	// unrelated reason, and must not be confused with a real confirmation).
	packetID64, perr := strconv.ParseUint(strings.TrimPrefix(packetIDHex, "0x"), 16, 32)
	if perr != nil {
		t.Fatalf("could not parse packet_id %q: %v", packetIDHex, perr)
	}
	if !result.PendingAckActive("A", uint32(packetID64)) {
		t.Fatalf("A's pending-ack for %s was cleared without a confirmation ever reaching "+
			"A (no message_delivered event at node A); pending_ack_remove should only run "+
			"from _handle_delivery_receipt when a receipt actually arrives at the source",
			packetIDHex)
	}
}
