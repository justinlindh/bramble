package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"net"
	"path/filepath"
	"testing"
	"time"
)

// fakeNode is an in-process emu-link client dialing the broker's unix socket:
// enough of the node side to exercise the broker (hello handshake, tx, and
// reading rx/txdone/cadres). It stands in for the real firmware binary, which
// another task builds.
type fakeNode struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

// dialFakeNode connects, completes the hello handshake at the current protocol
// version, and waits for the broker's time acknowledgement so the caller knows
// the node has attached (and bound its slot) before proceeding.
func dialFakeNode(t *testing.T, path, node string) *fakeNode {
	t.Helper()
	c, err := net.Dial("unix", path)
	if err != nil {
		t.Fatalf("dial %s: %v", path, err)
	}
	fn := &fakeNode{t: t, conn: c, r: bufio.NewReader(c)}
	fn.sendRaw(map[string]any{"t": "hello", "node": node, "version": EmuLinkVersion})
	msg := fn.expect(t, "time", 2*time.Second)
	if msg == nil {
		t.Fatalf("node %s: no time ack after hello", node)
	}
	return fn
}

func (fn *fakeNode) sendRaw(v any) {
	b, _ := json.Marshal(v)
	b = append(b, '\n')
	if _, err := fn.conn.Write(b); err != nil {
		fn.t.Fatalf("write: %v", err)
	}
}

// tx sends a tx message carrying payload (raw bytes, base64-encoded on the wire).
func (fn *fakeNode) tx(payload []byte) {
	fn.sendRaw(map[string]any{
		"t": "tx", "payload": base64.StdEncoding.EncodeToString(payload),
		"freq": 915000000, "sf": 10, "bw": 125000, "cr": 1, "power": 22,
	})
}

// read returns the next JSON message or nil on timeout.
func (fn *fakeNode) read(timeout time.Duration) map[string]any {
	fn.conn.SetReadDeadline(time.Now().Add(timeout))
	line, err := fn.r.ReadBytes('\n')
	if err != nil {
		return nil
	}
	var m map[string]any
	if err := json.Unmarshal(line, &m); err != nil {
		return nil
	}
	return m
}

// expect reads the next message and asserts its "t" discriminator.
func (fn *fakeNode) expect(t *testing.T, typ string, timeout time.Duration) map[string]any {
	t.Helper()
	m := fn.read(timeout)
	if m == nil {
		return nil
	}
	if m["t"] != typ {
		t.Fatalf("expected %q, got %v", typ, m)
	}
	return m
}

func (fn *fakeNode) close() { fn.conn.Close() }

// pumpUntil advances the sim clock in small steps, polling the collector fn
// after each step, until fn returns a message or the deadline passes. This
// mirrors the real-time loop (which advances against wall time) while keeping
// the test deterministic and fast: delivery events and txdone actions fire as
// soon as the clock passes their due time.
func pumpUntil(h *emuHarness, poll func() map[string]any) map[string]any {
	for i := 0; i < 200; i++ {
		h.advance(50_000) // 50 ms of sim time per step
		if m := poll(); m != nil {
			return m
		}
	}
	return nil
}

func TestExtNodeProtocolVersionMismatchRefused(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	c, err := net.Dial("unix", path)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.Close()
	b, _ := json.Marshal(map[string]any{"t": "hello", "node": "X", "version": EmuLinkVersion + 99})
	c.Write(append(b, '\n'))

	// The broker refuses the mismatch by closing the connection: a read returns
	// EOF rather than a time ack.
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 64)
	if n, err := c.Read(buf); err == nil && n > 0 {
		t.Fatalf("expected connection close on version mismatch, got %q", buf[:n])
	}
}

func TestExtNodeTxPricedAndDeliveredWithRSSI(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	// A at the origin, B 100 grid units away (well within the ~150-unit default
	// range). Attach in order so A binds slot 0 and B binds slot 1.
	h.reserveSlot(0, 0, "A")
	h.reserveSlot(100, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	payload := []byte("hello-bramble-16")
	nodeA.tx(payload)

	// B receives the frame with the model RSSI for a 100-unit link (-88 dBm,
	// per the log-distance path loss), payload intact.
	rx := pumpUntil(h, func() map[string]any { return nodeB.read(20 * time.Millisecond) })
	if rx == nil {
		t.Fatal("node B never received the frame")
	}
	if rx["t"] != "rx" {
		t.Fatalf("expected rx on B, got %v", rx)
	}
	got, err := base64.StdEncoding.DecodeString(rx["payload"].(string))
	if err != nil || string(got) != string(payload) {
		t.Fatalf("rx payload = %q (err %v), want %q", got, err, payload)
	}
	if rssi := int(rx["rssi"].(float64)); rssi != -88 {
		t.Fatalf("rx rssi = %d, want -88 (100-unit link)", rssi)
	}
	if freq := int(rx["freq"].(float64)); freq != 915000000 {
		t.Fatalf("rx freq = %d, want 915000000", freq)
	}

	// A gets a txdone priced at the deterministic time-on-air for the frame.
	td := pumpUntil(h, func() map[string]any { return nodeA.read(20 * time.Millisecond) })
	if td == nil || td["t"] != "txdone" {
		t.Fatalf("node A expected txdone, got %v", td)
	}
	if toa := uint32(td["toa_ms"].(float64)); toa != h.toaMs(len(payload)) {
		t.Fatalf("txdone toa_ms = %d, want %d (deterministic ToA)", toa, h.toaMs(len(payload)))
	}
}

func TestExtNodeCollisionOnOverlappingAirtime(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	h.setLBT(false) // let overlapping transmissions actually collide
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	// Receiver R at the origin; two transmitters A and B equidistant (50 units,
	// equal RSSI so neither captures) and audible at R.
	h.reserveSlot(0, 0, "R")
	h.reserveSlot(50, 0, "A")
	h.reserveSlot(0, 50, "B")
	nodeR := dialFakeNode(t, path, "R")
	defer nodeR.close()
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	// Both transmit at sim time 0 (the clock has not advanced yet): identical
	// air windows overlap. Give both readLoops a moment to fold the two
	// transmissions into the channel log before the clock moves.
	nodeA.tx([]byte("frame-from-a-xx"))
	nodeB.tx([]byte("frame-from-b-xx"))
	time.Sleep(80 * time.Millisecond)

	// Advance well past both air windows, then confirm R hears NOTHING: the two
	// overlapping frames destroyed each other at the receiver.
	for i := 0; i < 4; i++ {
		h.advance(1_000_000) // 4 s of sim time, covering any ToA + propagation
	}
	if m := nodeR.read(150 * time.Millisecond); m != nil && m["t"] == "rx" {
		t.Fatalf("R should hear nothing under collision, got rx %v", m)
	}
}

// TestExtNodeSingleTxNoCollisionDelivered is the positive control for the
// collision test: a lone transmitter reaches R cleanly under the same geometry.
func TestExtNodeSingleTxNoCollisionDelivered(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	h.setLBT(false)
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "R")
	h.reserveSlot(50, 0, "A")
	nodeR := dialFakeNode(t, path, "R")
	defer nodeR.close()
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()

	nodeA.tx([]byte("solo-frame-xxxx"))
	rx := pumpUntil(h, func() map[string]any {
		if m := nodeR.read(10 * time.Millisecond); m != nil && m["t"] == "rx" {
			return m
		}
		return nil
	})
	if rx == nil {
		t.Fatal("R should hear a lone transmission")
	}
}
