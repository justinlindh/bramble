package main

import "testing"

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
