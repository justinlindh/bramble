package main

import (
	"encoding/json"
	"os"
	"testing"
)

// receiptSend is one "packet_sent" event for a DELIVERY_RECEIPT (gosim's
// confirmation packet, the analogue of the firmware flooded ACK: the packet
// that feeds confirmed_delivery_rate).
type receiptSend struct {
	node string
	dest string
}

// runFloodAckScenario runs the SAME 3-hop line floodTransportLineScenario
// (A-B-C-D, D is 3 hops from A, out of direct radio range) that Task 1's test
// uses, and returns the confirmation-return-path evidence:
//   - confirmed: the final_metrics "confirmed" count (sender-confirmed
//     deliveries; this is exactly what confirmed_delivery_rate is built from);
//   - senderConfirmed: whether A (the ORIGINAL sender) observed the
//     confirmation (a message_delivered event at node "A", emitted by
//     _handle_delivery_receipt's consume branch when the receipt reaches the
//     originator);
//   - receiptSends: every DELIVERY_RECEIPT packet_sent, so a test can prove
//     the receipt travelled home by FLOODING (dest 0xFFFFFFFF) vs by a routed
//     unicast (dest == a specific next hop).
func runFloodAckScenario(t *testing.T, namePrefix string, floodTransport bool) (confirmed float64, senderConfirmed bool, receiptSends []receiptSend) {
	t.Helper()
	tmp, err := os.CreateTemp("", namePrefix+"-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.WriteString(floodTransportLineScenario(floodTransport)); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	tmp.Close()

	result, err := runScenarioHeadless(tmp.Name())
	if err != nil {
		t.Fatalf("runScenarioHeadless: %v", err)
	}

	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)

		switch typ {
		case "final_metrics":
			confirmed, _ = evt["confirmed"].(float64)
		case "message_delivered":
			// _handle_delivery_receipt's consume branch emits a
			// message_delivered at the ORIGINATOR when the receipt gets
			// home; on this line topology only A originates, so a
			// message_delivered at "A" is the sender observing confirmation.
			if node == "A" {
				senderConfirmed = true
			}
		case "packet_sent":
			if pt, _ := evt["pkt_type"].(string); pt == "DELIVERY_RECEIPT" {
				dest, _ := evt["dest"].(string)
				receiptSends = append(receiptSends, receiptSend{node: node, dest: dest})
			}
		}
	}
	return confirmed, senderConfirmed, receiptSends
}

// TestFloodedAckConfirmsSenderWithoutRoutes is Flooding F1 Task 2's
// system-level proof, driven through gosim's bridge.c (the REAL firmware flood
// engine): with flood_transport:true, D (3 hops from A) delivers A's unicast
// DATA and FLOODS the confirmation receipt back. Every relay rebroadcasts that
// receipt (dest 0xFFFFFFFF) through the same channel_flood_decide engine the
// DATA flood uses, never a routed unicast toward a next hop off a route table:
// the confirmation-return path consults NO route entries. A observes the
// confirmation and confirmed_delivery_rate registers it (confirmed >= 1).
//
// Compare TestFloodedAckOffRoutesReceipt below: identical topology/traffic
// with the toggle off returns the receipt as a routed unicast instead. That
// asymmetry is the toggle's own correctness proof, not "flooding happens to
// confirm here regardless".
func TestFloodedAckConfirmsSenderWithoutRoutes(t *testing.T) {
	confirmed, senderConfirmed, receiptSends := runFloodAckScenario(t, "flooded-ack-on", true)

	if len(receiptSends) == 0 {
		t.Fatalf("no DELIVERY_RECEIPT was ever sent: D never confirmed the delivery at all")
	}

	// Every confirmation-return send must be a FLOOD (broadcast, dest
	// 0xFFFFFFFF). If any receipt went to a specific next-hop address, a route
	// lookup (forward_data) produced it, which is exactly what Task 2 removes
	// from the confirmation path.
	sawFloodedReceipt := false
	for _, s := range receiptSends {
		if s.dest == "0xFFFFFFFF" {
			sawFloodedReceipt = true
		} else {
			t.Errorf("node %s sent the confirmation receipt to targeted unicast dest %s; under "+
				"flood_transport the receipt must FLOOD home (dest 0xFFFFFFFF) with no route "+
				"lookup", s.node, s.dest)
		}
	}
	if !sawFloodedReceipt {
		t.Fatalf("no relay/destination ever flooded the receipt (dest 0xFFFFFFFF); receipt sends "+
			"were: %+v", receiptSends)
	}

	if !senderConfirmed {
		t.Fatalf("A (the original sender) never observed the flooded confirmation receipt")
	}
	if confirmed < 1 {
		t.Fatalf("confirmed = %v, want >= 1: the flooded receipt must register in "+
			"confirmed_delivery_rate", confirmed)
	}
}

// TestFloodedAckOffRoutesReceipt is the A/B baseline: identical
// topology/traffic with flood_transport left at its default (off) returns the
// confirmation receipt via the pre-existing reactive route-lookup forward, a
// targeted unicast frame to the discovered next hop, never a broadcast. This
// is what proves flood_transport actually changes the CONFIRMATION transport
// (routed -> flooded), rather than the "on" run simply looking flooded
// regardless.
func TestFloodedAckOffRoutesReceipt(t *testing.T) {
	confirmed, senderConfirmed, receiptSends := runFloodAckScenario(t, "flooded-ack-off", false)

	if len(receiptSends) == 0 {
		t.Fatalf("no DELIVERY_RECEIPT was ever sent over the reactive path")
	}
	for _, s := range receiptSends {
		if s.dest == "0xFFFFFFFF" {
			t.Errorf("node %s broadcast the confirmation receipt (dest 0xFFFFFFFF) with "+
				"flood_transport off; the reactive path must forward it via a targeted unicast "+
				"next hop", s.node)
		}
	}
	// Reactive still confirms (via routes), so this is a genuine A/B: both
	// paths confirm, only the transport differs.
	if !senderConfirmed || confirmed < 1 {
		t.Fatalf("reactive path did not confirm: senderConfirmed=%v confirmed=%v", senderConfirmed,
			confirmed)
	}
}
