package websocket

import (
	"encoding/json"
	"fmt"
	"strings"
)

type broadcastDeliveryParams struct {
	BroadcastID   string   `json:"broadcastId"`
	PacketID      string   `json:"packetId"`
	From          string   `json:"from"`
	To            string   `json:"to"`
	Status        string   `json:"status"`
	HopCount      int      `json:"hopCount"`
	DeliveredAtMs uint64   `json:"deliveredAtMs"`
	Path          []string `json:"path,omitempty"`
}

type broadcastDeliveryNotification struct {
	JSONRPC string                  `json:"jsonrpc"`
	Method  string                  `json:"method"`
	Params  broadcastDeliveryParams `json:"params"`
}

// BuildBroadcastDeliveryNotification converts simulator message_delivered events
// into synthetic bramble.onBroadcastDelivery notifications.
func BuildBroadcastDeliveryNotification(raw []byte, telemetryMode string) ([]byte, bool) {
	if telemetryMode == "off" {
		return nil, false
	}

	var event map[string]any
	if err := json.Unmarshal(raw, &event); err != nil {
		return nil, false
	}

	if event["type"] != "message_delivered" {
		return nil, false
	}

	from, _ := event["from"].(string)
	if from == "" {
		// Reachability telemetry only: ignore local destination marker events.
		return nil, false
	}

	packetID, _ := event["packet_id"].(string)
	if packetID == "" {
		return nil, false
	}
	packetID = normalizeAddr(packetID)

	timestampUS, _ := event["timestamp_us"].(float64)
	hops := intValue(event["hops"])

	n := broadcastDeliveryNotification{
		JSONRPC: "2.0",
		Method:  "bramble.onBroadcastDelivery",
		Params: broadcastDeliveryParams{
			BroadcastID:   fmt.Sprintf("SIM-BCAST-%s", packetID),
			PacketID:      packetID,
			From:          normalizeAddr(from),
			To:            "FFFFFFFF",
			Status:        "delivered",
			HopCount:      hops,
			DeliveredAtMs: uint64(timestampUS / 1000.0),
		},
	}

	if telemetryMode != "recipient_only" {
		if rawPath, ok := event["path"].([]any); ok {
			path := make([]string, 0, len(rawPath))
			for _, v := range rawPath {
				if s, ok := v.(string); ok {
					path = append(path, normalizeAddr(s))
				}
			}
			n.Params.Path = path
		}
	}

	encoded, err := json.Marshal(n)
	if err != nil {
		return nil, false
	}
	return encoded, true
}

func intValue(v any) int {
	if f, ok := v.(float64); ok {
		return int(f)
	}
	return 0
}

func normalizeAddr(v string) string {
	return strings.TrimPrefix(strings.ToUpper(v), "0X")
}
