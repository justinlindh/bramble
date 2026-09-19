package main

import (
	"encoding/json"
	"testing"
)

// message_delivery_rate must be the end-to-end scripted-message outcome
// (delivered over terminal states), NOT a delivered/total_packets ratio
// against every frame of every type on the air: at 10 nodes the honest
// figure is 19/20 = 0.95 while the packet-count ratio reads 19/203 = 0.094
// against the same run.
func TestMessageDeliveryRateUsesTerminalMessageDenominator(t *testing.T) {
	if got := messageDeliveryRate(19, 0, 1); got != 0.95 {
		t.Fatalf("19 delivered / 20 terminal = 0.95, got %v", got)
	}
	if got := messageDeliveryRate(1, 19, 0); got != 0.05 {
		t.Fatalf("1 delivered / 20 terminal = 0.05, got %v", got)
	}
	if got := messageDeliveryRate(0, 0, 0); got != 0.0 {
		t.Fatalf("no scripted messages must report 0, got %v", got)
	}
}

// The periodic "metrics" tick must report message_delivery_rate under the
// same definition final_metrics uses, computed from that tick's own counters,
// so a live dashboard and an end-of-run report cannot disagree on the number.
func TestMetricsTickCarriesTheTerminalStateDeliveryRate(t *testing.T) {
	scenarioJSON := generateGridScenarioJSON(t, gridScenarioParams{
		Name:       "sf7-45u-25-tick-rate-test",
		NodeCount:  25,
		Spacing:    45,
		SF:         7,
		BWHz:       250000,
		Range:      58,
		DurationS:  300,
		MsgsPerMin: 5,
	})
	result := writeAndRunScenario(t, "sf7-45u-25-tick-rate-test", scenarioJSON)

	ticks, nonZero := 0, 0
	for _, line := range result.Lines() {
		var evt map[string]any
		if err := json.Unmarshal([]byte(line), &evt); err != nil || evt["type"] != "metrics" {
			continue
		}
		ticks++
		rate, ok := evt["message_delivery_rate"].(float64)
		if !ok {
			t.Fatalf("metrics tick has no message_delivery_rate: %s", line)
		}
		delivered, _ := evt["delivered"].(float64)
		dropped, _ := evt["dropped"].(float64)
		undelivered, _ := evt["undelivered"].(float64)
		want := messageDeliveryRate(uint64(delivered), uint64(dropped), uint64(undelivered))
		if diff := rate - want; diff > 1e-9 || diff < -1e-9 {
			t.Fatalf("tick message_delivery_rate = %v, want %v (delivered=%v dropped=%v undelivered=%v)",
				rate, want, delivered, dropped, undelivered)
		}
		if rate > 0 {
			nonZero++
		}
	}
	t.Logf("%d metrics ticks checked, %d with a nonzero delivery rate", ticks, nonZero)
	if ticks == 0 {
		t.Fatal("scenario emitted no metrics ticks")
	}
	if nonZero == 0 {
		t.Fatalf("all %d ticks reported a zero delivery rate; the scenario delivered nothing, "+
			"so the assertion above was never exercised on a real value", ticks)
	}
}
