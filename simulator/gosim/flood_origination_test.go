package main

import (
	"encoding/json"
	"os"
	"testing"
)

// originationEvidence captures, for the ORIGINAL sender "A" on the A-B-C-D line
// scenario, what Task 3 needs to prove send-side flood origination:
//   - dataSendDests: the dest of every DATA packet_sent A ORIGINATED (the first
//     send and any retry re-flood). A flood origination broadcasts (dest
//     0xFFFFFFFF, no route consulted); a reactive origination sends to a
//     route-resolved next hop. This asymmetry is the direct proof that the
//     toggle changes the ORIGINATION transport.
//   - senderConfirmed / confirmed: whether A observed the flooded confirmation
//     receipt come home (message_delivered at A) and confirmed_delivery_rate
//     registered it, i.e. the full origination-to-confirmation round trip
//     closed with EMPTY route tables.
type originationEvidence struct {
	dataSendDests   []string
	senderConfirmed bool
	confirmed       float64
}

func runFloodOriginationScenario(t *testing.T, namePrefix string, floodTransport bool) originationEvidence {
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

	var ev originationEvidence
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)

		switch typ {
		case "final_metrics":
			ev.confirmed, _ = evt["confirmed"].(float64)
		case "message_delivered":
			// On this line topology only A originates, so _handle_delivery_
			// receipt's consume branch emitting a message_delivered at "A" is A
			// observing the confirmation home.
			if node == "A" {
				ev.senderConfirmed = true
			}
		case "packet_sent":
			// A's own DATA sends only (node "A"); relay DATA sends come from B/C
			// and are covered by Task 1's relay test.
			if node != "A" {
				continue
			}
			if pt, _ := evt["pkt_type"].(string); pt == "DATA" {
				dest, _ := evt["dest"].(string)
				ev.dataSendDests = append(ev.dataSendDests, dest)
			}
		}
	}
	return ev
}

// TestFloodOriginationFloodsWithoutDiscovery is Flooding F1 Task 3's
// system-level proof, driven through gosim's bridge.c (the REAL firmware
// origination path with g_flood_transport_enabled). With flood_transport:true
// and EMPTY route tables (no scenario field pre-seeds a route), A originates
// its unicast DATA to D by FLOODING it immediately (broadcast, dest
// 0xFFFFFFFF) with no route lookup, the DATA reaches D 3 hops out, D floods a
// confirmation receipt back, and A ends confirmed. This is the whole
// origination-to-confirmation round trip with empty routes.
//
// Compare TestFloodOriginationOffRoutesFirst below: identical topology/traffic
// with the toggle off makes A originate the DATA to a route-resolved next hop
// instead. That asymmetry proves the toggle changes ORIGINATION, not that
// flooding happens to confirm regardless.
func TestFloodOriginationFloodsWithoutDiscovery(t *testing.T) {
	ev := runFloodOriginationScenario(t, "flood-origination-on", true)

	if len(ev.dataSendDests) == 0 {
		t.Fatalf("A never originated a DATA packet at all under the flood transport")
	}
	// Every DATA A originates (the first flood and any retry re-flood) must be
	// a broadcast (dest 0xFFFFFFFF). A targeted unicast dest would mean a route
	// lookup produced it, which is exactly what flood origination removes.
	for _, dest := range ev.dataSendDests {
		if dest != "0xFFFFFFFF" {
			t.Errorf("A originated DATA to targeted unicast dest %s under flood_transport; "+
				"origination must FLOOD (dest 0xFFFFFFFF) with no route lookup", dest)
		}
	}
	if !ev.senderConfirmed {
		t.Fatalf("A (the original sender) never observed the flooded confirmation for its " +
			"flood-originated DATA")
	}
	if ev.confirmed < 1 {
		t.Fatalf("confirmed = %v, want >= 1: the flood-originated round trip must register in "+
			"confirmed_delivery_rate", ev.confirmed)
	}
}

// TestFloodOriginationOffRoutesFirst is the A/B baseline: identical
// topology/traffic with flood_transport left at its default (off) makes A
// originate the DATA to a route-resolved next hop (never a broadcast). Reactive
// still confirms via routes, so both paths confirm; only the origination
// transport differs.
func TestFloodOriginationOffRoutesFirst(t *testing.T) {
	ev := runFloodOriginationScenario(t, "flood-origination-off", false)

	if len(ev.dataSendDests) == 0 {
		t.Fatalf("A never originated a DATA packet over the reactive path")
	}
	for _, dest := range ev.dataSendDests {
		if dest == "0xFFFFFFFF" {
			t.Errorf("A broadcast its DATA origination (dest 0xFFFFFFFF) with flood_transport off; " +
				"the reactive path must originate to a route-resolved unicast next hop")
		}
	}
	if !ev.senderConfirmed || ev.confirmed < 1 {
		t.Fatalf("reactive path did not confirm: senderConfirmed=%v confirmed=%v", ev.senderConfirmed,
			ev.confirmed)
	}
}
