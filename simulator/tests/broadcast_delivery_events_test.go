package tests

import (
	"encoding/json"
	"testing"

	"bramble-sim/websocket"
)

func decodeNotification(t *testing.T, data []byte) map[string]any {
	t.Helper()
	var v map[string]any
	if err := json.Unmarshal(data, &v); err != nil {
		t.Fatalf("unmarshal notification: %v", err)
	}
	return v
}

func TestBroadcastSendEmitsDeliveryEventsForReachableRecipients(t *testing.T) {
	raw := []byte(`{"type":"message_delivered","timestamp_us":1234000,"node":"A","packet_id":"0xA1B2C3D4","from":"0x11112222","hops":2,"path":["0x11112222","0x33334444"]}`)

	notification, ok := websocket.BuildBroadcastDeliveryNotification(raw, "full")
	if !ok {
		t.Fatalf("expected broadcast delivery notification")
	}

	decoded := decodeNotification(t, notification)
	if decoded["method"] != "bramble.onBroadcastDelivery" {
		t.Fatalf("expected method bramble.onBroadcastDelivery, got %v", decoded["method"])
	}

	params := decoded["params"].(map[string]any)
	if params["status"] != "delivered" {
		t.Fatalf("expected delivered status, got %v", params["status"])
	}
	path, ok := params["path"].([]any)
	if !ok || len(path) != 2 {
		t.Fatalf("expected path with 2 hops, got %#v", params["path"])
	}
}

func TestRecipientOnlyModeEmitsNoPathArray(t *testing.T) {
	raw := []byte(`{"type":"message_delivered","timestamp_us":1234000,"node":"A","packet_id":"0xDEADBEEF","from":"0x11112222","hops":1,"path":["0x11112222"]}`)

	notification, ok := websocket.BuildBroadcastDeliveryNotification(raw, "recipient_only")
	if !ok {
		t.Fatalf("expected broadcast delivery notification")
	}

	decoded := decodeNotification(t, notification)
	params := decoded["params"].(map[string]any)
	if _, exists := params["path"]; exists {
		t.Fatalf("expected no path in recipient_only mode, got %#v", params["path"])
	}
}
