package websocket

import (
	"encoding/json"
	"testing"
)

// decode unmarshals a built notification so tests can assert on fields without
// depending on JSON key ordering.
func decode(t *testing.T, raw []byte) broadcastDeliveryNotification {
	t.Helper()
	var n broadcastDeliveryNotification
	if err := json.Unmarshal(raw, &n); err != nil {
		t.Fatalf("unmarshal built notification: %v", err)
	}
	return n
}

func TestBuildBroadcastDeliveryNotification_Suppressed(t *testing.T) {
	// Inputs that must produce no notification (ok == false), for every reason
	// the builder bails out: telemetry off, malformed JSON, wrong event type,
	// and the two required-field guards.
	cases := []struct {
		name          string
		telemetryMode string
		raw           string
	}{
		{"telemetry off", "off", `{"type":"message_delivered","from":"0xAB","packet_id":"0x01"}`},
		{"invalid json", "full", `{not json`},
		{"wrong event type", "full", `{"type":"beacon_sent","from":"0xAB","packet_id":"0x01"}`},
		{"missing from", "full", `{"type":"message_delivered","packet_id":"0x01"}`},
		{"empty from", "full", `{"type":"message_delivered","from":"","packet_id":"0x01"}`},
		{"missing packet_id", "full", `{"type":"message_delivered","from":"0xAB"}`},
		{"empty packet_id", "full", `{"type":"message_delivered","from":"0xAB","packet_id":""}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			out, ok := BuildBroadcastDeliveryNotification([]byte(tc.raw), tc.telemetryMode)
			if ok {
				t.Fatalf("expected ok=false, got ok=true with output %s", out)
			}
			if out != nil {
				t.Fatalf("expected nil output, got %s", out)
			}
		})
	}
}

func TestBuildBroadcastDeliveryNotification_Full(t *testing.T) {
	raw := `{
		"type": "message_delivered",
		"from": "0xdeadbeef",
		"packet_id": "0x2a",
		"timestamp_us": 5000,
		"hops": 3,
		"path": ["0xaa", "BB", "0xCc"]
	}`
	out, ok := BuildBroadcastDeliveryNotification([]byte(raw), "full")
	if !ok {
		t.Fatal("expected ok=true")
	}
	n := decode(t, out)

	if n.JSONRPC != "2.0" {
		t.Errorf("jsonrpc = %q, want 2.0", n.JSONRPC)
	}
	if n.Method != "bramble.onBroadcastDelivery" {
		t.Errorf("method = %q, want bramble.onBroadcastDelivery", n.Method)
	}
	p := n.Params
	if p.PacketID != "2A" {
		t.Errorf("packetId = %q, want 2A (uppercased, 0x stripped)", p.PacketID)
	}
	if p.BroadcastID != "SIM-BCAST-2A" {
		t.Errorf("broadcastId = %q, want SIM-BCAST-2A", p.BroadcastID)
	}
	if p.From != "DEADBEEF" {
		t.Errorf("from = %q, want DEADBEEF", p.From)
	}
	if p.To != "FFFFFFFF" {
		t.Errorf("to = %q, want FFFFFFFF", p.To)
	}
	if p.Status != "delivered" {
		t.Errorf("status = %q, want delivered", p.Status)
	}
	if p.HopCount != 3 {
		t.Errorf("hopCount = %d, want 3", p.HopCount)
	}
	if p.DeliveredAtMs != 5 {
		t.Errorf("deliveredAtMs = %d, want 5 (timestamp_us/1000)", p.DeliveredAtMs)
	}
	want := []string{"AA", "BB", "CC"}
	if len(p.Path) != len(want) {
		t.Fatalf("path = %v, want %v", p.Path, want)
	}
	for i := range want {
		if p.Path[i] != want[i] {
			t.Errorf("path[%d] = %q, want %q", i, p.Path[i], want[i])
		}
	}
}

func TestBuildBroadcastDeliveryNotification_RecipientOnlyOmitsPath(t *testing.T) {
	raw := `{
		"type": "message_delivered",
		"from": "0xAB",
		"packet_id": "0x01",
		"timestamp_us": 12000,
		"hops": 1,
		"path": ["0xaa", "0xbb"]
	}`
	out, ok := BuildBroadcastDeliveryNotification([]byte(raw), "recipient_only")
	if !ok {
		t.Fatal("expected ok=true")
	}
	n := decode(t, out)
	if n.Params.Path != nil {
		t.Errorf("path = %v, want nil in recipient_only mode", n.Params.Path)
	}
	// The omitempty tag means the key must be absent, not just empty.
	if got := string(out); jsonHasKey(t, got, "path") {
		t.Errorf("recipient_only output should omit the path key entirely: %s", got)
	}
	if n.Params.DeliveredAtMs != 12 {
		t.Errorf("deliveredAtMs = %d, want 12", n.Params.DeliveredAtMs)
	}
}

func TestBuildBroadcastDeliveryNotification_EmptyModeIncludesPath(t *testing.T) {
	// Only "off" and "recipient_only" are special-cased; any other value
	// (including "" and "full") takes the path-including branch.
	raw := `{"type":"message_delivered","from":"0xAB","packet_id":"0x01","path":["0xaa"]}`
	for _, mode := range []string{"", "full", "unexpected"} {
		out, ok := BuildBroadcastDeliveryNotification([]byte(raw), mode)
		if !ok {
			t.Fatalf("mode %q: expected ok=true", mode)
		}
		n := decode(t, out)
		if len(n.Params.Path) != 1 || n.Params.Path[0] != "AA" {
			t.Errorf("mode %q: path = %v, want [AA]", mode, n.Params.Path)
		}
	}
}

func TestBuildBroadcastDeliveryNotification_NonStringPathEntriesDropped(t *testing.T) {
	// path is []any from the JSON decoder; non-string members are skipped.
	raw := `{"type":"message_delivered","from":"0xAB","packet_id":"0x01","path":["0xaa",42,"0xbb"]}`
	out, ok := BuildBroadcastDeliveryNotification([]byte(raw), "full")
	if !ok {
		t.Fatal("expected ok=true")
	}
	n := decode(t, out)
	want := []string{"AA", "BB"}
	if len(n.Params.Path) != len(want) {
		t.Fatalf("path = %v, want %v", n.Params.Path, want)
	}
	for i := range want {
		if n.Params.Path[i] != want[i] {
			t.Errorf("path[%d] = %q, want %q", i, n.Params.Path[i], want[i])
		}
	}
}

func TestBuildBroadcastDeliveryNotification_MissingOptionalFields(t *testing.T) {
	// timestamp_us and hops absent: both default to zero, and no path is set.
	raw := `{"type":"message_delivered","from":"0xAB","packet_id":"0x01"}`
	out, ok := BuildBroadcastDeliveryNotification([]byte(raw), "full")
	if !ok {
		t.Fatal("expected ok=true")
	}
	n := decode(t, out)
	if n.Params.HopCount != 0 {
		t.Errorf("hopCount = %d, want 0", n.Params.HopCount)
	}
	if n.Params.DeliveredAtMs != 0 {
		t.Errorf("deliveredAtMs = %d, want 0", n.Params.DeliveredAtMs)
	}
	if n.Params.Path != nil {
		t.Errorf("path = %v, want nil", n.Params.Path)
	}
}

func TestNormalizeAddr(t *testing.T) {
	cases := []struct{ in, want string }{
		{"0xab", "AB"},
		{"0XAB", "AB"},
		{"AB", "AB"},
		{"ab", "AB"},
		{"", ""},
		{"0x", ""},
		{"deadBEEF", "DEADBEEF"},
	}
	for _, tc := range cases {
		if got := normalizeAddr(tc.in); got != tc.want {
			t.Errorf("normalizeAddr(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestIntValue(t *testing.T) {
	if got := intValue(float64(7)); got != 7 {
		t.Errorf("intValue(7.0) = %d, want 7", got)
	}
	if got := intValue("not a number"); got != 0 {
		t.Errorf("intValue(string) = %d, want 0", got)
	}
	if got := intValue(nil); got != 0 {
		t.Errorf("intValue(nil) = %d, want 0", got)
	}
}

// jsonHasKey reports whether the top-level params object carries the given key,
// used to assert omitempty behavior that a decoded struct cannot distinguish.
func jsonHasKey(t *testing.T, raw, key string) bool {
	t.Helper()
	var outer struct {
		Params map[string]json.RawMessage `json:"params"`
	}
	if err := json.Unmarshal([]byte(raw), &outer); err != nil {
		t.Fatalf("unmarshal for key check: %v", err)
	}
	_, ok := outer.Params[key]
	return ok
}
