package main

import (
	"encoding/json"
	"os"
	"testing"
)

func TestPartitionMessagesKeepsTheTerminalStatesDisjoint(t *testing.T) {
	cases := []struct {
		name                                      string
		sent, delivered, droppedPreSend, failPost uint64
		want                                      messageOutcomes
	}{
		{"every message delivered", 20, 20, 0, 0, messageOutcomes{20, 20, 0, 0}},
		{"never-sent messages are outside sent", 6, 6, 14, 0, messageOutcomes{6, 6, 14, 0}},
		// A message abandoned after sending is one of the sent-not-delivered
		// messages: it moves from undelivered to dropped, it is not added twice.
		{"post-send failure leaves undelivered", 10, 4, 3, 2, messageOutcomes{10, 4, 5, 4}},
		{"undelivered floors at zero", 5, 5, 0, 1, messageOutcomes{5, 5, 1, 0}},
	}
	for _, c := range cases {
		got := partitionMessages(c.sent, c.delivered, c.droppedPreSend, c.failPost)
		if got != c.want {
			t.Errorf("%s: got %+v, want %+v", c.name, got, c.want)
		}
	}
}

// On a scenario with link loss, the radio layer loses far more frames than the
// scenario has messages. message_delivery_rate is a message figure, so its
// three terminal states must sum to the scripted message count however many
// frames the channel lost, and every scripted message must land in one of them,
// including one whose source node is killed while it is still looking for a
// route.
func TestTerminalStatesSumToTheScriptedMessagesUnderLinkLoss(t *testing.T) {
	raw, err := os.ReadFile("../scenarios/10-node-grid.json")
	if err != nil {
		t.Fatalf("read scenario: %v", err)
	}
	var scenario struct {
		Events []struct {
			Type string `json:"type"`
		} `json:"events"`
	}
	if err := json.Unmarshal(raw, &scenario); err != nil {
		t.Fatalf("parse scenario: %v", err)
	}
	scripted := 0
	for _, e := range scenario.Events {
		if e.Type == "send_message" {
			scripted++
		}
	}

	final := runAndGetFinalMetrics(t, "10-node-grid-outcomes", string(raw))
	num := func(key string) float64 {
		v, ok := final[key].(float64)
		if !ok {
			t.Fatalf("final_metrics has no numeric %q", key)
		}
		return v
	}
	delivered, dropped, undelivered := num("delivered"), num("dropped"), num("undelivered")
	framesLost := num("frames_lost")

	if sum := delivered + dropped + undelivered; int(sum) != scripted {
		t.Errorf("delivered %v + dropped %v + undelivered %v = %v, want the %d scripted messages",
			delivered, dropped, undelivered, sum, scripted)
	}
	if framesLost <= float64(scripted) {
		t.Fatalf("frames_lost = %v, not above the %d scripted messages: this scenario no longer "+
			"loses enough frames to tell a frame count from a message count", framesLost, scripted)
	}
	want := delivered / float64(scripted)
	if diff := num("message_delivery_rate") - want; diff > 1e-9 || diff < -1e-9 {
		t.Errorf("message_delivery_rate = %v, want delivered/scripted = %v", num("message_delivery_rate"), want)
	}
}
