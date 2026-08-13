package main

import (
	"encoding/json"
	"fmt"
	"testing"
)

// floodHopLimitLineScenario builds an N-node line (N0-N1-...-, 100-unit
// spacing) and floods ONE unicast DATA from N0 to the node at index dest under
// the REAL firmware flood transport (flood_transport:true, driven through
// bridge.c). At the sim's default SF10/125kHz range (~149.9 units) each node
// hears only its immediate neighbors, so the destination is exactly `dest`
// radio hops from N0. "collisions":false keeps it deterministic (the point is
// the hop-budget reach, not the MAC model), mirroring flood_transport_test.go.
//
// The optional flood_hop_limit field sets the flood-transport origination hop
// budget (firmware's s_flood_hop_limit): a flood reaches node k iff the hop
// limit >= k, so this scenario is the sweep vehicle for "raising the hop limit
// lets a flood reach a farther node the default could not".
func floodHopLimitLineScenario(nodeCount, dest, floodHopLimit int) string {
	type scenario struct {
		Name          string `json:"name"`
		Mode          string `json:"mode"`
		DurationMs    int    `json:"duration_ms"`
		FloodTranspt  bool   `json:"flood_transport"`
		FloodHopLimit *int   `json:"flood_hop_limit,omitempty"`
		Nodes         []any  `json:"nodes"`
		Radio         any    `json:"radio"`
		Events        []any  `json:"events"`
	}
	var nodes []any
	for i := 0; i < nodeCount; i++ {
		nodes = append(nodes, map[string]any{
			"id": fmt.Sprintf("N%d", i), "x": float64(i * 100), "y": 0.0,
		})
	}
	s := scenario{
		Name:         "flood-hop-limit-line",
		Mode:         "deterministic",
		DurationMs:   120000,
		FloodTranspt: true,
		Nodes:        nodes,
		Radio: map[string]any{
			"loss_pct":                      0,
			"propagation_speed_ms_per_unit": 0.1,
			"collisions":                    false,
		},
		Events: []any{
			map[string]any{
				"at_ms": 1000, "type": "send_message",
				"src": "N0", "dest": fmt.Sprintf("N%d", dest),
			},
		},
	}
	// Omit the field entirely to exercise the default (8) path; set it only
	// when a test explicitly sweeps it, mirroring floodTransportLineScenario's
	// omit-for-default convention.
	if floodHopLimit > 0 {
		v := floodHopLimit
		s.FloodHopLimit = &v
	}
	b, err := json.Marshal(s)
	if err != nil {
		panic(err)
	}
	return string(b)
}

// runFloodHopLimitScenario runs floodHopLimitLineScenario headlessly and
// returns whether the destination node received the message A originated.
func runFloodHopLimitScenario(t *testing.T, nodeCount, dest, floodHopLimit int) bool {
	t.Helper()
	result := writeAndRunScenario(t, "flood-hop-limit", floodHopLimitLineScenario(nodeCount, dest, floodHopLimit))

	destID := fmt.Sprintf("N%d", dest)
	var packetIDHex string
	delivered := false
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		typ, _ := evt["type"].(string)
		node, _ := evt["node"].(string)
		if typ == "message_sent" && node == "N0" {
			packetIDHex, _ = evt["packet_id"].(string)
		}
		if typ == "message_delivered" && node == destID {
			pid, _ := evt["packet_id"].(string)
			if pid != "" && pid == packetIDHex {
				delivered = true
			}
		}
	}
	if packetIDHex == "" {
		t.Fatalf("N0 never originated a flood DATA (no message_sent); lines:\n%s",
			joinLines(result.Lines()))
	}
	return delivered
}

// TestFloodHopLimitRaisingReachesFartherNode is Flooding F1 finalize's
// system-level proof that the flood hop limit is operator-settable and it is
// what sets reach: on a 12-node line, a flood from N0 to N10 (10 hops out)
// does NOT arrive at the default hop budget (8 < 10) but DOES arrive once the
// operator raises flood_hop_limit to 12 (>= 10). Same topology and traffic in
// both runs; only the hop budget changes, so the asymmetry is the config's own
// correctness proof, not a topology artifact.
func TestFloodHopLimitRaisingReachesFartherNode(t *testing.T) {
	const nodeCount = 12
	const dest = 10 // 10 radio hops from N0

	// Default budget (field omitted -> 8): N10 is out of reach.
	if runFloodHopLimitScenario(t, nodeCount, dest, 0) {
		t.Fatalf("N%d (%d hops) was reached at the default flood hop limit (8); the line is too "+
			"short to prove the hop budget bounds reach", dest, dest)
	}

	// Raised budget (12 >= 10): the same flood now reaches N10.
	if !runFloodHopLimitScenario(t, nodeCount, dest, 12) {
		t.Fatalf("N%d (%d hops) was NOT reached even with flood_hop_limit=12; raising the flood "+
			"hop budget must extend reach to a farther node", dest, dest)
	}
}

// TestFloodHopLimitDefaultReachesWithinBudget guards the other side: a node
// WITHIN the default 8-hop budget is reached with the field omitted, so the
// failure above is the hop budget bounding reach, not the flood being broken.
func TestFloodHopLimitDefaultReachesWithinBudget(t *testing.T) {
	const nodeCount = 12
	const dest = 6 // 6 hops, inside the default budget of 8
	if !runFloodHopLimitScenario(t, nodeCount, dest, 0) {
		t.Fatalf("N%d (%d hops, within the default 8-hop budget) was not reached at the default "+
			"flood hop limit; the flood transport is not delivering at all", dest, dest)
	}
}
