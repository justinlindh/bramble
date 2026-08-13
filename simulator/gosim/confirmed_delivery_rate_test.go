package main

import (
	"encoding/json"
	"fmt"
	"testing"
)

// TestConfirmedDeliveryRateMatchesReceiptsHome is Phase 2 "save reactive
// routing" Part A's positive proof: on the same 4-hop line topology
// confirmation_return_test.go uses (A-B-C-D, one scripted message A->D),
// the delivery receipt makes it all the way back to A (Phase 1's
// breadcrumb fix), so confirmed_delivery_rate must be > 0 and must equal
// the exact fraction of scripted messages whose receipt got home: here 1/1.
func TestConfirmedDeliveryRateMatchesReceiptsHome(t *testing.T) {
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

	finalMetrics := runAndGetFinalMetrics(t, "phase1-line-4hop", scenarioJSON)

	confirmed, _ := finalMetrics["confirmed"].(float64)
	confirmedRate, _ := finalMetrics["confirmed_delivery_rate"].(float64)
	deliveredRate, _ := finalMetrics["message_delivery_rate"].(float64)

	if confirmed != 1 {
		t.Fatalf("confirmed = %v, want 1 (A's receipt should return across the 3-hop line)",
			finalMetrics["confirmed"])
	}
	if confirmedRate <= 0 {
		t.Fatalf("confirmed_delivery_rate = %v, want > 0", confirmedRate)
	}
	// One scripted message, its receipt returned: confirmed_delivery_rate
	// must be exactly the fraction of scripted messages whose receipt made
	// it home (1/1 = 1.0), matching message_delivery_rate here since the
	// single message that reached the destination is also the one whose
	// receipt came back.
	if confirmedRate != 1.0 {
		t.Errorf("confirmed_delivery_rate = %v, want 1.0 (1/1 scripted message confirmed)",
			confirmedRate)
	}
	if confirmedRate != deliveredRate {
		t.Errorf("confirmed_delivery_rate (%v) should equal message_delivery_rate (%v) here: "+
			"the only delivered message is also the only confirmed one", confirmedRate, deliveredRate)
	}
}

// TestConfirmedDeliveryRateHonestUnderLoad is Phase 2 "save reactive
// routing" Part A's negative proof: at a saturated/loaded node count some
// scripted messages reach their destination (message_delivery_rate > 0)
// without their delivery receipt finding its way all the way back to the
// originator (confirmed_delivery_rate strictly lower). This is the exact
// gap Finding 2 (progress.md) identified: message_delivery_rate measures
// DESTINATION REACH, not sender confirmation, and overstates Bramble's
// actual differentiator (confirmed delivery) at scale.
//
// 50-node dense grid at SF7/250kHz (45-unit spacing with range pinned to 58
// units: orthogonal neighbors audible, diagonal ones at 63.6 units not), 5
// msgs/min traffic: a real, reproducible run where several messages reach
// their destination but not every one of those receipts makes it back
// through the loaded reverse path. (Re-tuned from 2 msgs/min when the
// Phase 4 identity-derived addresses reshuffled the deterministic
// collision pattern, and again from 3 msgs/min for issue #144: with the
// ACK retransmit ladder actually firing and destinations re-ACKing
// duplicate DATA, 3 msgs/min recovered every lost receipt and the gap
// closed at that load; 5 msgs/min keeps the reverse path lossy enough to
// exercise it.) The range is pinned rather than derived because this scenario
// is tuned to a specific reverse-path loss pattern: 58 units is what SF7/250 kHz
// derived to when it was tuned, and any value in [45, 63.6) gives the same
// orthogonal-only graph, so pinning it keeps the tuning independent of the
// derived-range calibration anchor (which tracks the frequency plan's default
// PHY, see radio_noise_margin_db).
func TestConfirmedDeliveryRateHonestUnderLoad(t *testing.T) {
	scenarioJSON := generateGridScenarioJSON(t, gridScenarioParams{
		Name:       "sf7-45u-50-confirmed-test",
		NodeCount:  50,
		Spacing:    45,
		SF:         7,
		BWHz:       250000,
		Range:      58,
		DurationS:  600,
		MsgsPerMin: 5,
	})

	finalMetrics := runAndGetFinalMetrics(t, "sf7-45u-50-confirmed-test", scenarioJSON)

	delivered, _ := finalMetrics["delivered"].(float64)
	confirmed, _ := finalMetrics["confirmed"].(float64)
	deliveredRate, _ := finalMetrics["message_delivery_rate"].(float64)
	confirmedRate, _ := finalMetrics["confirmed_delivery_rate"].(float64)

	if delivered <= 0 {
		t.Fatalf("delivered = %v, want > 0 (need at least one destination-reach to make this a "+
			"meaningful reach-vs-confirm comparison)", finalMetrics["delivered"])
	}
	if confirmed > delivered {
		t.Fatalf("confirmed (%v) > delivered (%v): a receipt cannot return for a message that "+
			"never reached its destination", confirmed, delivered)
	}
	if confirmedRate >= deliveredRate {
		t.Fatalf("confirmed_delivery_rate (%v) should be strictly less than message_delivery_rate "+
			"(%v) under load: some receipts should be lost on the way back even though their DATA "+
			"got through. If this now holds with equality, either the honest gap between reach and "+
			"confirmation has genuinely closed (re-tune this scenario to keep exercising it) or the "+
			"confirmed-tracking wiring regressed", confirmedRate, deliveredRate)
	}
	// The rate must be exactly confirmed / (delivered + dropped + undelivered),
	// the same terminal-state denominator message_delivery_rate uses.
	dropped, _ := finalMetrics["dropped"].(float64)
	undelivered, _ := finalMetrics["undelivered"].(float64)
	total := delivered + dropped + undelivered
	want := confirmed / total
	if diff := confirmedRate - want; diff > 1e-9 || diff < -1e-9 {
		t.Errorf("confirmed_delivery_rate = %v, want confirmed/total = %v/%v = %v",
			confirmedRate, confirmed, total, want)
	}
}

// runAndGetFinalMetrics writes scenarioJSON to a temp file, runs it
// headlessly, and returns the single final_metrics event's fields.
func runAndGetFinalMetrics(t *testing.T, namePrefix, scenarioJSON string) map[string]any {
	t.Helper()
	result := writeAndRunScenario(t, namePrefix, scenarioJSON)

	var finalMetrics map[string]any
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			continue
		}
		if evt["type"] == "final_metrics" {
			finalMetrics = evt
		}
	}
	if finalMetrics == nil {
		t.Fatalf("no final_metrics event emitted; lines:\n%s", joinLines(result.Lines()))
	}
	return finalMetrics
}

type gridScenarioParams struct {
	Name      string
	NodeCount int
	Spacing   float64
	SF        int
	BWHz      int
	// Range pins radio.range (grid units) instead of letting it derive from the
	// SF/BW link budget. Omit (0) to derive.
	Range      float64
	DurationS  int
	MsgsPerMin int
}

// generateGridScenarioJSON builds a grid-topology scenario JSON string
// in-process, mirroring simulator/scenarios/generate.py's grid_nodes and
// generate_messages (deterministic round-robin src/dest pairing), so gosim
// tests can exercise realistic multi-node scale scenarios without shelling
// out to the Python generator.
func generateGridScenarioJSON(t *testing.T, p gridScenarioParams) string {
	t.Helper()
	type node struct {
		ID string  `json:"id"`
		X  float64 `json:"x"`
		Y  float64 `json:"y"`
	}
	type event struct {
		AtMs int    `json:"at_ms"`
		Type string `json:"type"`
		Src  string `json:"src"`
		Dest string `json:"dest"`
	}
	type radio struct {
		LossPct                   float64 `json:"loss_pct"`
		PropagationSpeedMsPerUnit float64 `json:"propagation_speed_ms_per_unit"`
		SF                        int     `json:"sf,omitempty"`
		BWHz                      int     `json:"bw_hz,omitempty"`
		Range                     float64 `json:"range,omitempty"`
	}
	type scenario struct {
		Name       string  `json:"name"`
		Mode       string  `json:"mode"`
		DurationMs int     `json:"duration_ms"`
		Nodes      []node  `json:"nodes"`
		Radio      radio   `json:"radio"`
		Events     []event `json:"events"`
	}

	cols := 1
	for cols*cols < p.NodeCount {
		cols++
	}
	nodes := make([]node, p.NodeCount)
	for i := 0; i < p.NodeCount; i++ {
		row := i / cols
		col := i % cols
		nodes[i] = node{
			ID: nodeID(i + 1),
			X:  float64(col) * p.Spacing,
			Y:  float64(row) * p.Spacing,
		}
	}

	durationMs := p.DurationS * 1000
	var events []event
	if p.MsgsPerMin > 0 {
		msgIntervalMs := 60000.0 / float64(p.MsgsPerMin)
		timeMs := 10000.0
		msgID := 0
		for timeMs < float64(durationMs)-10000 {
			srcIdx := msgID % p.NodeCount
			destIdx := (msgID + p.NodeCount/2 + 1) % p.NodeCount
			if srcIdx != destIdx {
				events = append(events, event{
					AtMs: int(timeMs),
					Type: "send_message",
					Src:  nodes[srcIdx].ID,
					Dest: nodes[destIdx].ID,
				})
			}
			timeMs += msgIntervalMs
			msgID++
		}
	}

	s := scenario{
		Name:       p.Name,
		Mode:       "deterministic",
		DurationMs: durationMs,
		Nodes:      nodes,
		Radio: radio{
			LossPct:                   0,
			PropagationSpeedMsPerUnit: 0.1,
			SF:                        p.SF,
			BWHz:                      p.BWHz,
			Range:                     p.Range,
		},
		Events: events,
	}

	b, err := json.Marshal(s)
	if err != nil {
		t.Fatalf("marshal generated scenario: %v", err)
	}
	return string(b)
}

// nodeID matches generate.py's f"N{i+1:03d}" convention.
func nodeID(n int) string {
	return fmt.Sprintf("N%03d", n)
}
