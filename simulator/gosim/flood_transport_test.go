package main

import (
	"encoding/json"
	"testing"
)

// floodTransportLineScenario is a 4-node line (A-B-C-D, 100-unit spacing).
// At the sim's default SF9/125kHz derived range (150 units, see
// radio_derive_range in sim_radio.c), each node hears only its
// immediate neighbor: D is exactly 3 hops from A (A->B->C->D), out of direct
// radio range (300 units against a 150-unit range). A sends ONE unicast
// DATA to D. "collisions":false keeps this deterministic: the point of this
// test is the RELAY transport decision (flood vs reactive route-lookup
// forward) B and C make on receipt, not the MAC/collision model, mirroring
// intermediate_rrep_test.go's identical choice for the same reason.
//
// Task 1 is RECEIVE-side only (relay + deliver); send-side origination is
// Task 3 and is untouched here, so A still has to discover a route to D
// before it can originate the unicast DATA at all, exactly like the
// pre-Task-1 reactive path (bridge_handle_generate_message's route_lookup +
// RREQ/RREP fallback, main/mesh_task.c's mesh_send_message equivalent) --
// this happens identically in BOTH scenarios below and is not what
// flood_transport changes. What flood_transport changes is what B and C (the
// RELAYS) do with the DATA once it is in flight: reactive forwards it as a
// targeted unicast frame to the next hop off their route table; flood
// rebroadcasts it (dest 0xFFFFFFFF) through channel_flood_decide without
// ever consulting a route, the same engine the broadcast flood already uses.
// That relay-time broadcast-vs-unicast wire behavior is the proof used here
// (see runFloodTransportScenario / relayDataSends below); no scenario field
// exists to pre-seed a route table (every scenario here starts with none),
// so this is also the "empty route table" case the F1 plan asks for at the
// point the DATA is actually relayed.
func floodTransportLineScenario(floodTransport bool) string {
	type scenario struct {
		Name           string `json:"name"`
		Mode           string `json:"mode"`
		DurationMs     int    `json:"duration_ms"`
		FloodTransport *bool  `json:"flood_transport,omitempty"`
		Nodes          []any  `json:"nodes"`
		Radio          any    `json:"radio"`
		Events         []any  `json:"events"`
	}
	node := func(id string, x, y float64) map[string]any {
		return map[string]any{"id": id, "x": x, "y": y}
	}
	s := scenario{
		Name:       "flood-transport-line",
		Mode:       "deterministic",
		DurationMs: 30000,
		Nodes: []any{
			node("A", 0, 0),
			node("B", 100, 0),
			node("C", 200, 0),
			node("D", 300, 0),
		},
		Radio: map[string]any{
			"loss_pct":                      0,
			"propagation_speed_ms_per_unit": 0.1,
			"collisions":                    false,
		},
		Events: []any{
			map[string]any{"at_ms": 1000, "type": "send_message", "src": "A", "dest": "D"},
		},
	}
	// Only set the field when explicitly turning it on, so the "off" run
	// also proves the field-omitted default path (false), not just the
	// explicit-false path -- mirrors intermediateRREPStarScenario's identical
	// convention.
	if floodTransport {
		v := true
		s.FloodTransport = &v
	}
	b, err := json.Marshal(s)
	if err != nil {
		panic(err)
	}
	return string(b)
}

// relayDataSend is one "packet_sent" event for pkt_type DATA originated by a
// RELAY (any node other than the original sender "A").
type relayDataSend struct {
	node string
	dest string
}

// runFloodTransportScenario runs floodTransportLineScenario(floodTransport)
// headlessly and returns: whether D received the message (message_delivered
// for node "D" with the packet_id A originated), and every relay-originated
// DATA packet_sent event (node != "A").
func runFloodTransportScenario(t *testing.T, namePrefix string, floodTransport bool) (delivered bool, relaySends []relayDataSend) {
	t.Helper()
	result := writeAndRunScenario(t, namePrefix, floodTransportLineScenario(floodTransport))

	var packetIDHex string
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)

		if typ == "message_sent" && node == "A" {
			packetIDHex, _ = evt["packet_id"].(string)
		}
		if typ == "packet_sent" && node != "A" {
			pktType, _ := evt["pkt_type"].(string)
			if pktType == "DATA" {
				dest, _ := evt["dest"].(string)
				relaySends = append(relaySends, relayDataSend{node: node, dest: dest})
			}
		}
		if typ == "message_delivered" && node == "D" {
			pid, _ := evt["packet_id"].(string)
			if pid != "" && pid == packetIDHex {
				delivered = true
			}
		}
	}

	if packetIDHex == "" {
		t.Fatalf("A never sent a unicast DATA message to D (no message_sent event seen); lines:\n%s",
			joinLines(result.Lines()))
	}
	return delivered, relaySends
}

// TestFloodTransportRelaysAsBroadcastNotUnicastForward is Flooding F1 Task
// 1's system-level proof, driven through gosim's bridge.c (the REAL firmware
// flood decide, not the Go-only floodSim MODEL): with flood_transport:true,
// B and C relay the unicast DATA A->D by REBROADCASTING it (dest_addr
// 0xFFFFFFFF) through the SAME flood engine (channel_flood_decide) the
// broadcast flood already uses, never as a targeted unicast forward to a
// specific next hop -- proving they flood, they do not route it, and D still
// receives it 3 hops out. Compare against
// TestFloodTransportOffRelaysAsUnicastForward below, identical
// topology/traffic, which relays via targeted unicast instead: that
// asymmetry is the toggle's own correctness proof, not just "flooding
// happens to work here".
func TestFloodTransportRelaysAsBroadcastNotUnicastForward(t *testing.T) {
	delivered, relaySends := runFloodTransportScenario(t, "flood-transport-on", true)

	if !delivered {
		t.Fatalf("D (3 hops from A) never received the unicast DATA under the flood transport")
	}
	if len(relaySends) == 0 {
		t.Fatalf("no relay ever re-sent the DATA packet; B and/or C never relayed it at all")
	}

	sawBroadcastRelay := false
	for _, s := range relaySends {
		if s.dest == "0xFFFFFFFF" {
			sawBroadcastRelay = true
		} else {
			t.Errorf("relay %s sent DATA to targeted unicast dest %s; under flood_transport every "+
				"relay must rebroadcast (dest 0xFFFFFFFF) through channel_flood_decide, never "+
				"forward via a route lookup", s.node, s.dest)
		}
	}
	if !sawBroadcastRelay {
		t.Fatalf("no relay ever rebroadcast the DATA (dest 0xFFFFFFFF); relay sends were: %+v",
			relaySends)
	}
}

// TestFloodTransportOffRelaysAsUnicastForward is the A/B baseline: identical
// topology/traffic with flood_transport left at its default (omitted, i.e.
// false) relays via the pre-existing reactive route-lookup forward, a
// targeted unicast frame to the discovered next hop, never a broadcast. This
// is what proves flood_transport actually changes relay behavior, rather
// than the "on" run simply happening to look this way regardless.
func TestFloodTransportOffRelaysAsUnicastForward(t *testing.T) {
	delivered, relaySends := runFloodTransportScenario(t, "flood-transport-off", false)

	if !delivered {
		t.Fatalf("D (3 hops from A) never received the unicast DATA over the reactive path")
	}
	if len(relaySends) == 0 {
		t.Fatalf("no relay ever forwarded the DATA packet; B and/or C never forwarded it at all")
	}

	for _, s := range relaySends {
		if s.dest == "0xFFFFFFFF" {
			t.Errorf("relay %s broadcast the DATA (dest 0xFFFFFFFF) with flood_transport off; "+
				"the reactive path must only ever forward via a targeted unicast next hop", s.node)
		}
	}
}
