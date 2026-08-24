package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math/rand"
	"net"
	"path/filepath"
	"sync"
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
func (fn *fakeNode) tx(payload []byte) { fn.txPHY(payload, 10, 125000) }

// txPHY is tx with an explicit reported PHY, for the adoption tests: the real
// firmware reports whatever mesh_init_radio_config left in its radio config.
func (fn *fakeNode) txPHY(payload []byte, sf, bwHz int) {
	fn.sendRaw(map[string]any{
		"t": "tx", "payload": base64.StdEncoding.EncodeToString(payload),
		"freq": 915000000, "sf": sf, "bw": bwHz, "cr": 1, "power": 22,
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

// The ether prices airtime at the PHY the attached firmware reports, whatever
// the C radio model's own default is. That default is the same frequency-plan
// PHY a stock node boots on (SF9/125 kHz), so this test starts the ether at
// SF10 to make adoption observable: that is also the concrete case adoption
// has to cover, a node whose PHY differs from the ether's (an NVS-overridden
// sf/bw, or a build for another region's plan).
func TestExtNodeAdoptsReportedPHY(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}
	h.reserveSlot(0, 0, "A")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()

	h.setEtherPHY(10, 125000)
	sf10Toa := h.toaMs(60)
	nodeA.txPHY(make([]byte, 60), 9, 125000)

	td := pumpUntil(h, func() map[string]any { return nodeA.read(20 * time.Millisecond) })
	if td == nil || td["t"] != "txdone" {
		t.Fatalf("node A expected txdone, got %v", td)
	}
	if sf := int(h.sim.radio.sf); sf != 9 {
		t.Fatalf("ether sf = %d after a node reported SF9, want 9", sf)
	}
	sf9Toa := h.toaMs(60)
	if toa := uint32(td["toa_ms"].(float64)); toa != sf9Toa {
		t.Fatalf("txdone toa_ms = %d, want %d (priced at the reported SF9)", toa, sf9Toa)
	}
	// The whole point: SF9 is materially cheaper than SF10, so pricing at the
	// reported PHY keeps a short beacon cadence from oversubscribing the
	// emulated channel.
	if sf9Toa >= sf10Toa {
		t.Fatalf("SF9 ToA %d ms is not below the SF10 ToA %d ms", sf9Toa, sf10Toa)
	}
	// Coding rate is a ToA input too, so it is adopted alongside SF/BW.
	if cr := int(h.sim.radio.cr); cr != 1 {
		t.Fatalf("ether cr = %d after a node reported CR 1, want 1", cr)
	}
}

// Coding rate rides the same adoption: a node on CR 4/8 makes the ether price
// its frames at CR 4/8, so the priced ToA keeps agreeing with radio_airtime.c.
func TestExtNodeAdoptsReportedCodingRate(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}
	h.reserveSlot(0, 0, "A")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()

	h.setEtherPHY(10, 125000)
	cr45Toa := h.toaMs(60)
	nodeA.sendRaw(map[string]any{
		"t": "tx", "payload": base64.StdEncoding.EncodeToString(make([]byte, 60)),
		"freq": 915000000, "sf": 10, "bw": 125000, "cr": 4, "power": 22,
	})
	if td := pumpUntil(h, func() map[string]any { return nodeA.read(20 * time.Millisecond) }); td == nil {
		t.Fatal("node A expected txdone")
	}
	if cr := int(h.sim.radio.cr); cr != 4 {
		t.Fatalf("ether cr = %d after a node reported CR 4, want 4", cr)
	}
	if cr48Toa := h.toaMs(60); cr48Toa <= cr45Toa {
		t.Fatalf("CR 4/8 ToA %d ms is not above the CR 4/5 default %d ms", cr48Toa, cr45Toa)
	}
}

// A scenario that pins radio.sf owns the PHY: an attached node never overrides
// it, so scenarios written against a deliberate PHY keep reproducing.
func TestExtNodePinnedScenarioPHYWins(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	// A scenario that pinned radio.sf: SF10, deliberately not the model default,
	// so "the pin held" cannot be confused with "nothing happened".
	h.sim.emuPHYPinned = true
	h.setEtherPHY(10, 125000)
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}
	h.reserveSlot(0, 0, "A")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()

	nodeA.txPHY(make([]byte, 60), 7, 250000)
	if td := pumpUntil(h, func() map[string]any { return nodeA.read(20 * time.Millisecond) }); td == nil {
		t.Fatal("node A expected txdone")
	}
	if sf := int(h.sim.radio.sf); sf != 10 {
		t.Fatalf("pinned ether sf = %d after a node reported SF7, want the scenario's 10", sf)
	}
}

// scenarioPinsPHY drives that ownership: only an explicit sf/bw_hz counts.
func TestScenarioPinsPHY(t *testing.T) {
	cases := []struct {
		name string
		json string
		want bool
	}{
		{"no radio block", `{"name":"x"}`, false},
		{"radio without phy", `{"radio":{"range":150,"loss_pct":0}}`, false},
		{"radio with sf", `{"radio":{"range":150,"sf":7}}`, true},
		{"radio with bw_hz", `{"radio":{"range":150,"bw_hz":250000}}`, true},
		{"unparseable", `{`, false},
	}
	for _, c := range cases {
		if got := scenarioPinsPHY([]byte(c.json)); got != c.want {
			t.Errorf("%s: scenarioPinsPHY = %v, want %v", c.name, got, c.want)
		}
	}
}

func TestExtNodeCollisionOnOverlappingAirtime(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	h.disableLBT() // let overlapping transmissions actually collide
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
	h.disableLBT()
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

// TestBrokerFindByNodeRouting is a fast, same-package regression test for the
// gosim button-routing fix (Broker.findByNode, extnode.go): it must resolve a
// bound emu-link hello id to that node's own connection, resolve distinct ids
// to distinct connections, and return nil (not a stale/wrong connection) for
// an id nothing has ever attached under. No firmware node, no browser --
// this alone previously required the full E2E suite to exercise.
func TestBrokerFindByNodeRouting(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	h.reserveSlot(10, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	ecA := h.sim.broker.findByNode("A")
	if ecA == nil || ecA.node != "A" {
		t.Fatalf("findByNode(A) = %v, want the connection bound to A", ecA)
	}
	ecB := h.sim.broker.findByNode("B")
	if ecB == nil || ecB.node != "B" {
		t.Fatalf("findByNode(B) = %v, want the connection bound to B", ecB)
	}
	if ecA == ecB {
		t.Fatal("findByNode(A) and findByNode(B) returned the same connection")
	}

	// An id nothing ever attached under must resolve to nil, not a stale hit.
	if ec := h.sim.broker.findByNode("nonexistent"); ec != nil {
		t.Fatalf("findByNode(nonexistent) = %v, want nil", ec)
	}
	if ec := h.sim.broker.findByNode(""); ec != nil {
		t.Fatalf("findByNode(\"\") = %v, want nil", ec)
	}
}

// TestCmdButtonRoutesToCorrectNodeOnly exercises the actual bug fix end to
// end at the Go level: a "btn" Command (sim.go's cmdButton, wired into
// handleCommand's "btn" case) must reach ONLY the external firmware
// connection bound to the target node id, via sendButton, and must not
// fan out to any other attached node. It also covers the pre-fix failure
// shape (an unknown node id) as a silent no-op rather than a panic or a
// misdelivery.
func TestCmdButtonRoutesToCorrectNodeOnly(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	h.reserveSlot(10, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	h.sim.handleCommand(Command{Type: "btn", Node: "A", BtnID: "select", Edge: "down"})

	btn := nodeA.read(2 * time.Second)
	if btn == nil {
		t.Fatal("node A never received the button edge")
	}
	if btn["t"] != "btn" || btn["id"] != "select" || btn["edge"] != "down" {
		t.Fatalf("node A btn = %v, want t=btn id=select edge=down", btn)
	}

	// The OTHER attached node must see nothing: routing must hit exactly the
	// bound connection, not fan out or hit the wrong one.
	if m := nodeB.read(200 * time.Millisecond); m != nil {
		t.Fatalf("node B should not receive A's button edge, got %v", m)
	}

	// An id with no attached connection is a silent no-op: no panic, and it
	// must not deliver to either attached node.
	h.sim.handleCommand(Command{Type: "btn", Node: "does-not-exist", BtnID: "up", Edge: "up"})
	if m := nodeA.read(150 * time.Millisecond); m != nil {
		t.Fatalf("node A should not receive a button meant for an unknown node, got %v", m)
	}
	if m := nodeB.read(150 * time.Millisecond); m != nil {
		t.Fatalf("node B should not receive a button meant for an unknown node, got %v", m)
	}
}

// dialAndHello is a lower-level, *testing.T-free version of dialFakeNode's
// dial+handshake, safe to call from a background goroutine (t.Fatalf must
// only ever be called from the test's own goroutine; this returns an error
// instead so goroutines can report failures back through a channel).
func dialAndHello(path, node string) (net.Conn, error) {
	c, err := net.Dial("unix", path)
	if err != nil {
		return nil, err
	}
	b, _ := json.Marshal(map[string]any{"t": "hello", "node": node, "version": EmuLinkVersion})
	if _, err := c.Write(append(b, '\n')); err != nil {
		c.Close()
		return nil, err
	}
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	line, err := bufio.NewReader(c).ReadBytes('\n')
	if err != nil {
		c.Close()
		return nil, err
	}
	var m map[string]any
	if err := json.Unmarshal(line, &m); err != nil || m["t"] != "time" {
		c.Close()
		return nil, fmt.Errorf("node %s: unexpected hello ack %v (err %v)", node, m, err)
	}
	return c, nil
}

// TestBrokerFindByNodeConcurrentWithHelloBind is a -race-friendly test for
// the lock discipline findByNode and the hello-bind path share (both guarded
// by Broker.mu, see extnode.go's handleHello and findByNode comments): a pool
// of readers hammer findByNode (including for ids not yet attached) while
// every reserved slot's node concurrently dials in and completes its hello
// handshake. `go test -race` catches any unguarded access; the final
// resolution check catches any bind that got lost or misrouted under
// contention.
func TestBrokerFindByNodeConcurrentWithHelloBind(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	const n = 16
	ids := make([]string, n)
	for i := 0; i < n; i++ {
		ids[i] = fmt.Sprintf("N%d", i)
		h.reserveSlot(float32(i), 0, ids[i])
	}

	stop := make(chan struct{})
	var readers sync.WaitGroup
	readers.Add(1)
	go func() {
		defer readers.Done()
		for {
			select {
			case <-stop:
				return
			default:
				h.sim.broker.findByNode(ids[rand.Intn(n)])
				h.sim.broker.findByNode("nonexistent")
			}
		}
	}()

	type dialResult struct {
		conn net.Conn
		err  error
	}
	results := make([]dialResult, n)
	var dialers sync.WaitGroup
	for i := 0; i < n; i++ {
		dialers.Add(1)
		go func(i int) {
			defer dialers.Done()
			c, err := dialAndHello(path, ids[i])
			results[i] = dialResult{conn: c, err: err}
		}(i)
	}
	dialers.Wait()
	close(stop)
	readers.Wait()

	defer func() {
		for _, r := range results {
			if r.conn != nil {
				r.conn.Close()
			}
		}
	}()

	for i, r := range results {
		if r.err != nil {
			t.Fatalf("node %s: dial/hello failed: %v", ids[i], r.err)
		}
	}

	// Every id must now resolve to its own distinct, correctly-bound
	// connection: the concurrent readers must not have observed (or caused)
	// a bind to land on the wrong slot.
	seen := make(map[*extConn]string, n)
	for _, id := range ids {
		ec := h.sim.broker.findByNode(id)
		if ec == nil || ec.node != id {
			t.Fatalf("findByNode(%s) = %v, want the connection bound to %s", id, ec, id)
		}
		if other, dup := seen[ec]; dup {
			t.Fatalf("findByNode(%s) and findByNode(%s) returned the same connection", id, other)
		}
		seen[ec] = id
	}
}
