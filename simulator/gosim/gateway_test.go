package main

import (
	"bufio"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeSerial stands in for a real passthrough-mode node reached over serial.
// It speaks the JSON-RPC phy.* protocol the firmware exposes (DESIGN.md
// section 10): it answers phy.enable (optionally refusing until forced, exactly
// like a node that holds a live identity), records phy.tx frames the gateway
// pushes down for the real radio, and can emit bramble.onPhyFrame notifications
// (a real-mesh reception) up the link. It runs over an in-memory net.Pipe, so
// the test drives the gateway with zero real hardware.
type fakeSerial struct {
	conn net.Conn
	r    *bufio.Reader

	wmu  sync.Mutex
	txCh chan []byte // phy.tx frames, decoded from hex

	mu      sync.Mutex
	enables []bool // force flags seen, in order

	refuseUnlessForce bool
}

func newFakeSerial(conn net.Conn, refuseUnlessForce bool) *fakeSerial {
	fs := &fakeSerial{
		conn:              conn,
		r:                 bufio.NewReader(conn),
		txCh:              make(chan []byte, 16),
		refuseUnlessForce: refuseUnlessForce,
	}
	go fs.readLoop()
	return fs
}

func (fs *fakeSerial) writeLine(v interface{}) {
	b, _ := json.Marshal(v)
	b = append(b, '\n')
	fs.wmu.Lock()
	defer fs.wmu.Unlock()
	_, _ = fs.conn.Write(b)
}

func (fs *fakeSerial) readLoop() {
	for {
		line, err := fs.r.ReadBytes('\n')
		if err != nil {
			return
		}
		var m struct {
			ID     uint64 `json:"id"`
			Method string `json:"method"`
			Params struct {
				Force bool   `json:"force"`
				Frame string `json:"frame"`
			} `json:"params"`
		}
		if json.Unmarshal(line, &m) != nil {
			continue
		}
		switch m.Method {
		case "phy.enable":
			fs.mu.Lock()
			fs.enables = append(fs.enables, m.Params.Force)
			fs.mu.Unlock()
			enabled, requiresForce := true, false
			if fs.refuseUnlessForce && !m.Params.Force {
				enabled, requiresForce = false, true
			}
			fs.writeLine(map[string]interface{}{
				"jsonrpc": "2.0", "id": m.ID,
				"result": map[string]interface{}{
					"enabled": enabled, "requires_force": requiresForce,
					"ttl_s": 1800, "remaining_s": 1800,
				},
			})
		case "phy.tx":
			raw, err := hex.DecodeString(m.Params.Frame)
			if err == nil {
				select {
				case fs.txCh <- raw:
				default:
				}
			}
			fs.writeLine(map[string]interface{}{
				"jsonrpc": "2.0", "id": m.ID,
				"result": map[string]interface{}{"ok": true, "len": len(raw)},
			})
		}
	}
}

// emitFrame pushes a real-mesh reception up the link as the firmware would.
func (fs *fakeSerial) emitFrame(frame []byte, rssi, snr int, freqHz int64) {
	fs.writeLine(map[string]interface{}{
		"jsonrpc": "2.0", "method": "bramble.onPhyFrame",
		"params": map[string]interface{}{
			"frame": hex.EncodeToString(frame), "rssi": rssi, "snr": snr, "freq": freqHz,
		},
	})
}

func (fs *fakeSerial) enableForces() []bool {
	fs.mu.Lock()
	defer fs.mu.Unlock()
	return append([]bool(nil), fs.enables...)
}

// waitForLine polls the harness's captured UI-event stream for substr.
func waitForLine(h *emuHarness, substr string, d time.Duration) bool {
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		h.mu.Lock()
		for _, l := range h.lines {
			if strings.Contains(l, substr) {
				h.mu.Unlock()
				return true
			}
		}
		h.mu.Unlock()
		time.Sleep(2 * time.Millisecond)
	}
	return false
}

// startGateway wires a gateway to the broker at path over an in-memory serial
// link and runs its bridge in the background. It returns the fake node so the
// test can drive the serial side, and waits until the gateway has attached to
// the ether (broker node_joined) so subsequent onPhyFrame emissions are picked
// up by the running pump rather than swallowed by the enable handshake.
func startGateway(t *testing.T, h *emuHarness, path, name string, refuseUnlessForce bool) *fakeSerial {
	t.Helper()
	gwEnd, fakeEnd := net.Pipe()
	fs := newFakeSerial(fakeEnd, refuseUnlessForce)

	g := NewGateway(path, "")
	g.nodeName = name
	go func() { _ = g.bridge(gwEnd) }()
	t.Cleanup(func() { gwEnd.Close(); fakeEnd.Close() })

	if !waitForLine(h, `"node":"`+name+`"`, 3*time.Second) {
		t.Fatalf("gateway %q never attached to the ether", name)
	}
	return fs
}

// TestGatewayForwardsRealFrameToEther is the phase-1 exit behavior in
// microcosm: a frame the real hardware hears (delivered as bramble.onPhyFrame
// over serial) is injected into the ether by the gateway and arrives on a
// virtual node, payload intact.
func TestGatewayForwardsRealFrameToEther(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	// Virtual receiver B binds its reserved slot first; the gateway attaches
	// afterward and lands at the origin (within range of B).
	h.reserveSlot(50, 0, "B")
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	fs := startGateway(t, h, path, "gateway", false)
	if forces := fs.enableForces(); len(forces) != 1 || forces[0] {
		t.Fatalf("expected one unforced phy.enable, got %v", forces)
	}

	payload := []byte("from-the-real-fleet")
	fs.emitFrame(payload, -70, 8, 915000000)

	rx := pumpUntil(h, func() map[string]any { return nodeB.read(20 * time.Millisecond) })
	if rx == nil || rx["t"] != "rx" {
		t.Fatalf("virtual node B never received the bridged frame, got %v", rx)
	}
	got, err := base64.StdEncoding.DecodeString(rx["payload"].(string))
	if err != nil || string(got) != string(payload) {
		t.Fatalf("bridged payload = %q (err %v), want %q", got, err, payload)
	}
}

// TestGatewayForwardsEtherFrameToRadio is the reverse direction: a frame a
// virtual node transmits reaches the gateway's ether slot and is pushed back
// down the serial link as phy.tx for the real radio.
func TestGatewayForwardsEtherFrameToRadio(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(50, 0, "B")
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	fs := startGateway(t, h, path, "gateway", false)

	payload := []byte("reply-to-the-fleet")
	nodeB.tx(payload)

	// Advance the sim clock until the gateway pushes the frame down as phy.tx.
	var got []byte
	for i := 0; i < 200 && got == nil; i++ {
		h.advance(50_000)
		select {
		case got = <-fs.txCh:
		default:
			time.Sleep(2 * time.Millisecond)
		}
	}
	if got == nil {
		t.Fatal("gateway never transmitted the ether frame out phy.tx")
	}
	if string(got) != string(payload) {
		t.Fatalf("phy.tx frame = %q, want %q", got, payload)
	}
}

// TestGatewayEscalatesToForceWhenIdentityHeld verifies the gating-aware
// handshake: a node that refuses the first (unforced) phy.enable because it
// holds a live identity is retried with force:true, and the gateway then
// attaches and bridges normally.
func TestGatewayEscalatesToForceWhenIdentityHeld(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(50, 0, "B")
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	fs := startGateway(t, h, path, "gateway", true) // refuse until forced

	forces := fs.enableForces()
	if len(forces) != 2 || forces[0] || !forces[1] {
		t.Fatalf("expected [false,true] enable sequence (refuse then force), got %v", forces)
	}

	// And the bridge still works after the forced enable.
	payload := []byte("forced-bridge-ok")
	fs.emitFrame(payload, -60, 9, 915000000)
	rx := pumpUntil(h, func() map[string]any { return nodeB.read(20 * time.Millisecond) })
	if rx == nil || rx["t"] != "rx" {
		t.Fatalf("no delivery after forced enable, got %v", rx)
	}
}
