package main

import (
	"encoding/json"
	"os"
	"strconv"
	"strings"
	"testing"
)

// TestPhase1ConfirmationReachesOriginatorAcrossMultiHopLine is the
// system-level proof for Phase 1 delivery-core plan Task 4
// the internal design plan.
//
// gosim used to install a route to every beacon sender (the old
// _handle_beacon in bridge.c), which firmware's own handle_beacon
// (main/mesh_task.c) never does. That accidentally supplied the reverse-hop
// route real relays never get, which masked the confirmation-return bug:
// relays only ever installed routes TOWARD an RREQ/RREP discovery target
// (rrep_rx_decide), never back toward a message's originator. Task 1
// removed that masking and captured the bug as a TDD baseline (this test
// used to assert sourceConfirmed == false, with an explicit BUG marker).
//
// Task 4 fixes the root cause: DATA now carries a relay-mutated prev_hop
// (wire v4), and every node that receives or forwards a DATA frame learns a
// route back to its src_addr via prev_hop (data_rx_decide's
// install_reverse_route, wired into both main/mesh_task.c's
// mesh_process_rx_packet and gosim's own _handle_data). On this same line
// topology forcing 3 hops (A-B-C-D), A's unicast DATA to D is still
// delivered (D decodes it), and D's delivery receipt now has a breadcrumb
// route at every relay to travel home on, so A observes the confirmation.
//
// This test also covers the Task 4 brief's ACK-loss fallback ("drop the
// first ACK, assert retry eventually yields DELIVERED; if Task 6 [re-ACK on
// duplicate DATA] not yet done, assert at least that the reverse route
// exists and a re-sent ACK would route"). Task 6 has since landed on the
// firmware side (main/mesh_task.c: a duplicate unicast DATA whose
// (src_addr, packet_id) was already delivered locally now re-sends the ACK
// instead of being silently dropped -- see components/dedup's
// dedup_contains and test_dedup.c's
// test_delivered_dedup_enables_reack_without_redelivery), but the sim
// harness still has no scenario primitive to selectively drop a single
// in-flight packet (only a global radio.loss_pct; gosim also does not
// dedup unicast DATA at the destination at all -- see bridge.c's dedup
// comment -- so it never had this gap to begin with, and there is nothing
// for gosim to reproduce here), so a literal "drop the first ACK, observe a
// second one arrive" run is still not constructible here. Per the brief's
// explicit fallback, the assertions below instead prove the weaker,
// still-load-bearing claim directly off this same run: every relay on the
// forward path (B, C) installs a reverse route to A as a side effect of
// A's DATA transiting it, BEFORE any delivery receipt is ever sent. That
// route does not depend on a receipt having arrived, which is exactly why
// it would still be there for a retried/re-sent confirmation even if the
// first attempt were lost.
//
// (These fallback assertions are folded into this same test, reusing one
// runScenarioHeadless call, rather than a second scenario-level test: the
// pipe-based stdout capture runScenarioHeadless uses is only built to be
// exercised once per test process, and a second back-to-back invocation
// in the same package process was observed to lose C-side fprintf lines,
// a pre-existing harness fragility this task does not attempt to fix.)
func TestPhase1ConfirmationReachesOriginatorAcrossMultiHopLine(t *testing.T) {
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
	var addrAHex string
	destDelivered := false
	sourceConfirmed := false
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)

		if typ == "node_joined" && node == "A" {
			addrAHex, _ = evt["addr"].(string)
		}

		if !strings.HasPrefix(typ, "message_") {
			continue
		}
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

	/* FIXED (Task 4): wire v4's relay-mutated prev_hop plus data_rx_decide's
	 * reverse-route learning give every relay on the forward path a
	 * breadcrumb route home, so D's delivery receipt now reaches A. */
	if !sourceConfirmed {
		t.Fatalf("source A never observed a delivery confirmation for %s even though D "+
			"decoded it. Task 4 (wire v4 prev_hop reverse-route learning) should make every "+
			"relay on the forward path (B, C) learn a route back to A via prev_hop as A's "+
			"DATA transits them, so D's delivery receipt has a route home", packetIDHex)
	}

	// The scenario's 15s duration is chosen to land well before the
	// NORMAL-tier pending-ack retry backoff exhausts (base 2s, doubling,
	// max 3 attempts: exhaustion lands around t=~20s after send at t=1s),
	// so a cleared pending ack here reflects "a confirmation genuinely
	// arrived" (pending_ack_remove only runs from _handle_delivery_receipt
	// when a receipt actually reaches the source), not "gave up after
	// retries" (which would also clear it, for an unrelated reason, and
	// must not be confused with a real confirmation: sourceConfirmed above
	// already rules that out, since only an arriving receipt sets it).
	packetID64, perr := strconv.ParseUint(strings.TrimPrefix(packetIDHex, "0x"), 16, 32)
	if perr != nil {
		t.Fatalf("could not parse packet_id %q: %v", packetIDHex, perr)
	}
	if result.PendingAckActive("A", uint32(packetID64)) {
		t.Fatalf("A's pending-ack for %s is still active even though A observed a delivery "+
			"confirmation (sourceConfirmed); _handle_delivery_receipt should have cleared it "+
			"via pending_ack_remove when the receipt arrived", packetIDHex)
	}

	// ACK-loss fallback (see the doc comment above): the reverse route at
	// every relay must exist independently of any receipt having arrived,
	// so it is unaffected by whether the first delivery confirmation made
	// it back or not. Read the C route table directly (RouteNextHop)
	// rather than the route_added JSON log line, which is emitted via the
	// same pipe-based stdout capture PendingAckActive's doc comment already
	// treats as reliable only for the higher-level message_* events this
	// test depends on above.
	if addrAHex == "" {
		t.Fatalf("never saw a node_joined event for A; could not determine A's address")
	}
	addrA64, perr := strconv.ParseUint(strings.TrimPrefix(addrAHex, "0x"), 16, 32)
	if perr != nil {
		t.Fatalf("could not parse A's address %q: %v", addrAHex, perr)
	}
	addrA := uint32(addrA64)
	for _, relay := range []string{"B", "C"} {
		if _, ok := result.RouteNextHop(relay, addrA); !ok {
			t.Fatalf("relay %s never installed a reverse route to A (%s) while forwarding "+
				"A's DATA. This route must exist independently of any delivery receipt "+
				"arriving, so a retried/re-sent confirmation can still find its way home "+
				"even if the first one is lost in flight", relay, addrAHex)
		}
	}
}
