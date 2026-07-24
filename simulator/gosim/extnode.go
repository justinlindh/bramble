package main

/*
#include "bridge.h"
#include <stdlib.h>
*/
import "C"
import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"syscall"
	"time"
)

// EmuLinkVersion is the emu-link wire protocol version the broker speaks
// (DESIGN.md section 8). It rides in every node's hello; a node that reports a
// different version is refused loudly (the broker logs the mismatch and closes
// the connection) rather than silently mis-parsing a future dialect.
const EmuLinkVersion = 1

// emuInbound is the union of every node->broker message shape (DESIGN.md
// section 8). "t" discriminates; the other fields are populated per type. A
// flat struct keeps decoding a single json.Unmarshal and makes unknown message
// types trivially ignorable (an unrecognized "t" simply matches no case).
type emuInbound struct {
	T       string `json:"t"`
	Node    string `json:"node"`    // hello: node id
	Version int    `json:"version"` // hello: protocol version
	Payload string `json:"payload"` // tx: base64 PHY payload
	Freq    int    `json:"freq"`    // tx: carrier (Hz), echoed back on rx
	SF      int    `json:"sf"`      // tx: spreading factor (adopted; see adoptReportedPHY)
	BW      int    `json:"bw"`      // tx: bandwidth (adopted; see adoptReportedPHY)
	CR      int    `json:"cr"`      // tx: coding rate (adopted; see adoptReportedPHY)
	Power   int    `json:"power"`   // tx: dBm (advisory: link budget stays scenario-owned)
	Seq     int    `json:"seq"`     // fb: frame sequence
	Kind    string `json:"kind"`    // fb: "partial" | "full"
	FB      string `json:"fb"`      // fb: base64 packed 1bpp framebuffer (opaque)
	FBW     int    `json:"w"`       // fb: panel width in px (250 e-paper, 128 OLED)
	FBH     int    `json:"h"`       // fb: panel height in px (122 e-paper, 64 OLED)
	BusyMs  int    `json:"busy_ms"` // fb: engine-computed panel busy duration
	LED     bool   `json:"led"`     // ind: notification LED state
	Buzzer  int    `json:"buzzer_hz"`
	Vibra   bool   `json:"vibra"`
	On      bool   `json:"on"`   // gpsgate: power-gate state
	Line    string `json:"line"` // log: one console line
}

// Broker->node message envelopes (DESIGN.md section 8). Each is marshaled as
// one JSON line onto the target connection's buffered send channel.
type rxMsg struct {
	T       string `json:"t"`
	Payload string `json:"payload"`
	RSSI    int    `json:"rssi"`
	SNR     int    `json:"snr"`
	Freq    int    `json:"freq"`
}

type txdoneMsg struct {
	T     string `json:"t"`
	ToaMs uint32 `json:"toa_ms"`
}

type cadresMsg struct {
	T    string `json:"t"`
	Busy bool   `json:"busy"`
}

type btnMsg struct {
	T    string `json:"t"`
	ID   string `json:"id"`   // "up" | "down" | "select" | "reset"
	Edge string `json:"edge"` // "down" | "up"
}

type timeMsg struct {
	T       string `json:"t"`
	EpochMs int64  `json:"epoch_ms"`
}

type nmeaMsg struct {
	T        string `json:"t"`
	Sentence string `json:"sentence"`
}

// extSlot is one external-node position reserved in the ether. The supervisor
// reserves a slot per firmware-node instance before spawning it (so a restart
// rebinds to the same position and reuses the same sim_node array entry, which
// is what keeps the node count bounded across resets); direct-dial tests
// reserve slots the same way. nodeIndex is -1 until the first hello binds a
// connection and creates the backing sim_node.
type extSlot struct {
	x, y      float32
	label     string
	nodeDir   string
	nodeIndex int
	addr      uint32
	conn      *extConn
	// boundID is the emu-link hello id of the node currently (or most recently)
	// bound to this slot, guarded by Broker.mu. The supervisor tags this slot's
	// console lines with it so console events carry the node's real address, not
	// the process label, which is what lets a multi-group scenario route consoles
	// correctly without the UI's label-suffix heuristic. It survives a restart
	// (identity persists), so a reset node's boot lines tag to the stable id even
	// before its next hello re-binds the slot.
	boundID string
}

// extConn is a single attached external node: its socket, a buffered outbound
// channel drained by a dedicated writer goroutine (so the simulation loop never
// blocks on a slow node's socket while holding s.mu), and its bound slot.
type extConn struct {
	broker *Broker
	conn   net.Conn
	send   chan []byte
	slot   *extSlot

	node string // hello id
	addr uint32

	// GPS feed state, guarded by sim.mu. gpsGen invalidates any scheduled
	// feed action from a previous gate-on interval: the action captures the
	// generation it was scheduled under and goes inert if it no longer
	// matches (gate cycled off, or off and on again).
	gpsOn  bool
	gpsGen uint64

	closeOnce sync.Once
	closed    chan struct{}
}

// Broker is the emu-link listener and the ether-side of the protocol. One
// broker per Sim owns the unix socket, accepts node connections, folds each
// attached node into the existing radio model as a sim_node, and routes tx/rx/
// txdone/cad traffic between the node processes and the C radio engine.
type Broker struct {
	sim  *Sim
	path string
	ln   net.Listener

	mu       sync.Mutex
	slots    []*extSlot
	conns    map[*extConn]bool
	attachCh chan *extConn // buffered; supervisor waits on it to sequence spawns
	stopped  bool
}

// NewBroker opens the emu-link unix socket at path (removing any stale socket
// file first) and returns a broker bound to sim. Call Start to begin accepting.
func NewBroker(sim *Sim, path string) (*Broker, error) {
	if path == "" {
		return nil, fmt.Errorf("emu-link: empty socket path")
	}
	_ = os.Remove(path) // clear a stale socket from a previous run
	ln, err := net.Listen("unix", path)
	if err != nil {
		return nil, fmt.Errorf("emu-link listen %s: %w", path, err)
	}
	return &Broker{
		sim:      sim,
		path:     path,
		ln:       ln,
		conns:    make(map[*extConn]bool),
		attachCh: make(chan *extConn, 64),
	}, nil
}

// Start launches the accept loop.
func (b *Broker) Start() { go b.acceptLoop() }

// Addr returns the socket path node processes should dial (the EMU_BROKER env).
func (b *Broker) Addr() string { return b.path }

// Stop closes the listener and every live connection and removes the socket.
func (b *Broker) Stop() {
	b.mu.Lock()
	if b.stopped {
		b.mu.Unlock()
		return
	}
	b.stopped = true
	ln := b.ln
	var conns []*extConn
	for c := range b.conns {
		conns = append(conns, c)
	}
	b.mu.Unlock()

	if ln != nil {
		ln.Close()
	}
	for _, c := range conns {
		c.close()
	}
	_ = os.Remove(b.path)
}

// resetSlots clears the reserved-slot list (called on scenario reload). Live
// connections are left untouched; they free their own slot on close.
func (b *Broker) resetSlots() {
	b.mu.Lock()
	b.slots = nil
	b.mu.Unlock()
}

// reserveSlot reserves a position in the ether for one external node and
// returns its slot. The supervisor calls this before spawning each firmware
// instance; tests call it before dialing a fake node.
func (b *Broker) reserveSlot(x, y float32, label, nodeDir string) *extSlot {
	slot := &extSlot{x: x, y: y, label: label, nodeDir: nodeDir, nodeIndex: -1}
	b.mu.Lock()
	b.slots = append(b.slots, slot)
	b.mu.Unlock()
	return slot
}

// slotBoundID returns the hello id most recently bound to slot, or "" if no
// node has attached to it yet. Read under b.mu so it races cleanly with the
// hello handler that sets it; the supervisor calls it to tag console lines.
func (b *Broker) slotBoundID(slot *extSlot) string {
	if slot == nil {
		return ""
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	return slot.boundID
}

// findByNode returns the live connection whose emu-link hello id is node, or
// nil if none is attached (a stale id from before a reset/reattach, or a
// scenario reload that tore the connection down). Used to route a UI face-
// button edge (Command.Type "btn") to the right external firmware process.
func (b *Broker) findByNode(node string) *extConn {
	if node == "" {
		return nil
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	for c := range b.conns {
		if c.node == node {
			return c
		}
	}
	return nil
}

func (b *Broker) acceptLoop() {
	for {
		conn, err := b.ln.Accept()
		if err != nil {
			b.mu.Lock()
			stopped := b.stopped
			b.mu.Unlock()
			if stopped {
				return
			}
			continue
		}
		ec := &extConn{
			broker: b,
			conn:   conn,
			send:   make(chan []byte, 256),
			closed: make(chan struct{}),
		}
		b.mu.Lock()
		b.conns[ec] = true
		b.mu.Unlock()
		go ec.writeLoop()
		go ec.readLoop()
	}
}

// bindSlot picks the oldest unbound reserved slot (FIFO) for a freshly
// hello'd connection, or, if none is free (e.g. an ad-hoc direct dial), creates
// one at the origin. Must be called with b.mu held.
func (b *Broker) bindSlot(ec *extConn) *extSlot {
	for _, s := range b.slots {
		if s.conn == nil {
			s.conn = ec
			return s
		}
	}
	s := &extSlot{nodeIndex: -1, conn: ec}
	b.slots = append(b.slots, s)
	return s
}

// writeLoop drains the send channel to the socket.
func (ec *extConn) writeLoop() {
	w := bufio.NewWriter(ec.conn)
	for {
		select {
		case <-ec.closed:
			return
		case msg, ok := <-ec.send:
			if !ok {
				return
			}
			if _, err := w.Write(msg); err != nil {
				ec.close()
				return
			}
			if err := w.Flush(); err != nil {
				ec.close()
				return
			}
		}
	}
}

// readLoop reads one JSON object per line and dispatches it.
func (ec *extConn) readLoop() {
	defer ec.close()
	sc := bufio.NewScanner(ec.conn)
	sc.Buffer(make([]byte, 64*1024), 1<<20)
	for sc.Scan() {
		line := sc.Bytes()
		if len(line) == 0 {
			continue
		}
		var msg emuInbound
		if err := json.Unmarshal(line, &msg); err != nil {
			log.Printf("emu-link: bad json from %s: %v", ec.label(), err)
			continue
		}
		ec.dispatch(&msg)
	}
}

func (ec *extConn) label() string {
	if ec.node != "" {
		return ec.node
	}
	if ec.slot != nil && ec.slot.label != "" {
		return ec.slot.label
	}
	return "extnode"
}

// dispatch routes one decoded inbound message. Unknown "t" values are ignored
// (forward compatibility). The hello handshake must complete before any other
// message is honored.
func (ec *extConn) dispatch(msg *emuInbound) {
	if msg.T == "hello" {
		ec.handleHello(msg)
		return
	}
	if ec.slot == nil {
		// Not attached yet: only hello is valid before the handshake.
		return
	}
	switch msg.T {
	case "tx":
		ec.handleTx(msg)
	case "cad":
		ec.handleCad()
	case "fb":
		ec.handleFB(msg)
	case "ind":
		ec.handleInd(msg)
	case "gpsgate":
		ec.handleGpsGate(msg)
	case "log":
		ec.handleLog(msg)
	default:
		// Unknown type: ignore (phase 2 forward compatibility).
	}
}

// handleHello validates the protocol version, binds the connection to a slot,
// folds it into the radio model as a sim_node, and acknowledges with a time
// message. A version mismatch is refused loudly and the connection is closed.
func (ec *extConn) handleHello(msg *emuInbound) {
	if msg.Version != EmuLinkVersion {
		log.Printf("emu-link: node %q protocol version %d != broker %d, refusing",
			msg.Node, msg.Version, EmuLinkVersion)
		ec.close()
		return
	}
	b := ec.broker
	s := b.sim

	s.mu.Lock()
	b.mu.Lock()
	slot := b.bindSlot(ec)
	slot.boundID = msg.Node // console tagging reads this under b.mu
	ec.node = msg.Node      // findByNode (btn routing) reads this under b.mu too
	b.mu.Unlock()

	ec.slot = slot
	slot.label = firstNonEmpty(slot.label, msg.Node)

	if slot.nodeIndex < 0 {
		id := msg.Node
		if id == "" {
			id = slot.label
		}
		idx := nodeArrayAdd(&s.nodes, id, 0, slot.x, slot.y)
		if idx < 0 {
			b.mu.Lock()
			slot.conn = nil
			b.mu.Unlock()
			s.mu.Unlock()
			log.Printf("emu-link: node array full, cannot attach %q", msg.Node)
			ec.close()
			return
		}
		node := C.node_array_get(&s.nodes, C.int(idx))
		slot.nodeIndex = idx
		slot.addr = uint32(node.addr)
	} else {
		// Reattach after a restart (reset): reuse the same sim_node entry so the
		// node count stays bounded, re-activating it at its slot position.
		node := C.node_array_get(&s.nodes, C.int(slot.nodeIndex))
		node.active = C.bool(true)
		node.x = C.float(slot.x)
		node.y = C.float(slot.y)
		node.tx_busy_until_us = 0
	}
	ec.addr = slot.addr
	s.extConns[ec.addr] = ec
	s.emitJSON(map[string]interface{}{
		"type": "node_joined", "timestamp_us": s.simTime,
		"node": ec.label(), "addr": fmt.Sprintf("0x%08X", ec.addr),
		"x": slot.x, "y": slot.y, "kind": "firmware",
	})
	s.mu.Unlock()

	// Acknowledge attach with the wall-clock epoch anchor (broker->node time),
	// so the node can align its clock at boot.
	ec.sendJSON(timeMsg{T: "time", EpochMs: time.Now().UnixMilli()})

	select {
	case b.attachCh <- ec:
	default:
	}
	log.Printf("emu-link: node %q attached as 0x%08X at (%.0f,%.0f)",
		ec.label(), ec.addr, slot.x, slot.y)
}

// handleTx folds an external node's transmission into the radio model: it
// prices a deterministic time-on-air, records the channel-occupancy window,
// schedules EVT_RECEIVE_PACKET for every in-range node (harness and external),
// and schedules the txdone acknowledgement for after the airtime elapses on the
// simulation clock. The payload is opaque bytes; it is delivered PHY-broadcast
// (every audible node receives it, exactly like a real LoRa transmission).
func (ec *extConn) handleTx(msg *emuInbound) {
	payload, err := base64.StdEncoding.DecodeString(msg.Payload)
	if err != nil {
		log.Printf("emu-link: bad tx payload from %s: %v", ec.label(), err)
		return
	}
	if len(payload) == 0 {
		return
	}
	if len(payload) > 256 {
		payload = payload[:256]
	}
	s := ec.broker.sim
	s.mu.Lock()
	defer s.mu.Unlock()
	if msg.Freq != 0 {
		s.emuFreq = msg.Freq // single-channel ether: remember the carrier for rx
	}
	ec.adoptReportedPHY(msg)
	node := C.node_array_find_by_addr(&s.nodes, C.uint32_t(ec.addr))
	if node == nil {
		return
	}
	now := s.simTime

	var pkt C.outbound_packet_t
	n := len(payload)
	pkt.len = C.uint16_t(n)
	pkt.is_broadcast = C.bool(true)
	pkt.dest_addr = C.uint32_t(0xFFFFFFFF)
	pkt.pkt_type = C.uint8_t(C.PKT_TYPE_DATA)
	for i := 0; i < n; i++ {
		pkt.data[i] = C.uint8_t(payload[i])
	}

	toaMs := uint32(C.radio_frame_airtime_ms(&s.radio, C.uint16_t(n)))
	toaUs := uint64(C.radio_frame_airtime_us(&s.radio, C.uint16_t(n)))

	C.sim_radio_broadcast(node, &pkt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics,
		C.uint64_t(now))

	// A deterministic transmit event for headless assertions and the UI: an
	// external firmware node's PHY frames are opaque to the broker (it never
	// decodes them), so the C engine emits no message_* event for them the way it
	// does for sim_node harness traffic. This is the greppable "node X keyed the
	// channel" signal that a scenario or smoke test keys off.
	s.emitJSON(map[string]interface{}{
		"type": "emu_tx", "node": ec.label(), "addr": fmt.Sprintf("0x%08X", ec.addr),
		"len": n, "toa_ms": toaMs,
	})

	// txdone rides the sim clock after the (deterministic) airtime, so the node
	// returns to RX at the right moment. The toa VALUE never depends on wall
	// time; only when the message is delivered does.
	s.scheduleBrokerAction(now+toaUs, func() {
		ec.sendJSON(txdoneMsg{T: "txdone", ToaMs: toaMs})
	})
}

// adoptReportedPHY points the ether's time-on-air model at the LoRa PHY the
// attached firmware node actually configured, which it reports on every tx.
//
// The broker used to discard these fields as "advisory" and price every frame
// at the C radio model's own default (SF10/125 kHz, chosen to mirror the
// firmware's RADIO_PROFILE_LONG_RANGE table). The running firmware does not use
// that table's SF: mesh_init_radio_config overwrites the profile's sf/bw_hz
// with the frequency plan's defaults, and every shipped plan (US915/EU868/
// AU915) defaults to SF9/125 kHz. Every emulated frame was therefore charged
// about twice its true airtime (732 ms vs about 371 ms for a 60-byte beacon),
// which is what tipped the emulator scenarios' budget-exempt short beacon
// cadence (EMU_BEACON_INTERVAL_MS, 2.6-4.1 s) from a busy channel into an
// oversubscribed one: a 3-node cell offered about 89% of channel capacity, so
// under the half-duplex + any-overlap-collision model the wait for a receiver's
// first clean beacon became a heavy-tailed lottery instead of a bounded time.
//
// Learning the PHY from the node rather than restating it in each scenario is
// what keeps the two from drifting apart again: a frequency-plan or profile
// change moves the ether with the firmware, automatically. A scenario that
// pins radio.sf or radio.bw_hz still owns its PHY and is never overridden.
// Adoption happens on the first tx, which is the boot beacon, so it lands
// before any reception the scenario cares about; the emulator scenarios all set
// an explicit "range", so the derived-range path cannot shift underneath them.
//
// Covers every input radio_frame_airtime_us reads: SF, bandwidth, and coding
// rate. Reported tx power is deliberately NOT adopted, because it feeds the
// derived-range link budget rather than airtime, and the ether's topology is
// the scenario's to declare. Must be called with s.mu held.
func (ec *extConn) adoptReportedPHY(msg *emuInbound) {
	s := ec.broker.sim
	if s.emuPHYPinned || msg.SF == 0 || msg.BW == 0 {
		return
	}
	if s.emuPHYAdopted {
		if msg.SF != s.emuPHYSF || msg.BW != s.emuPHYBWHz {
			// Single-channel ether: one PHY for everyone. Report the split
			// rather than let the last transmitter silently reprice the air.
			log.Printf("emu-link: node %q reports SF%d/%d Hz but the ether is SF%d/%d Hz; "+
				"keeping the ether PHY (nodes must share one PHY)",
				ec.label(), msg.SF, msg.BW, s.emuPHYSF, s.emuPHYBWHz)
		}
		return
	}
	s.emuPHYAdopted = true
	s.emuPHYSF = msg.SF
	s.emuPHYBWHz = msg.BW
	s.radio.sf = C.uint8_t(msg.SF)
	s.radio.bw_hz = C.uint32_t(msg.BW)
	if msg.CR != 0 {
		s.radio.cr = C.uint8_t(msg.CR)
	}
	log.Printf("emu-link: ether PHY adopted from node %q: SF%d BW %d Hz CR 4/%d",
		ec.label(), msg.SF, msg.BW, 4+msg.CR)
}

// handleCad answers a channel-activity-detection request. The broker models CAD
// as deterministic energy detection within the range disk: busy if any other
// node is mid-transmission (tx_busy_until_us in the future) and audible.
func (ec *extConn) handleCad() {
	s := ec.broker.sim
	s.mu.Lock()
	busy := s.channelBusyFor(ec.addr)
	s.mu.Unlock()
	ec.sendJSON(cadresMsg{T: "cadres", Busy: busy})
}

// handleFB stores the latest framebuffer for this node and forwards it to UI
// subscribers. The framebuffer bytes are opaque to the broker (the frontend
// renders e-paper physics); only the latest frame per node is kept.
func (ec *extConn) handleFB(msg *emuInbound) {
	s := ec.broker.sim
	s.emitJSON(map[string]interface{}{
		"type": "device_fb", "node": ec.label(), "addr": fmt.Sprintf("0x%08X", ec.addr),
		"seq": msg.Seq, "kind": msg.Kind, "fb": msg.FB, "busy_ms": msg.BusyMs,
		"w": msg.FBW, "h": msg.FBH,
	})
}

// handleInd forwards an indicator (LED / buzzer / vibra) state change to the UI.
func (ec *extConn) handleInd(msg *emuInbound) {
	s := ec.broker.sim
	s.emitJSON(map[string]interface{}{
		"type": "device_ind", "node": ec.label(), "addr": fmt.Sprintf("0x%08X", ec.addr),
		"led": msg.LED, "buzzer_hz": msg.Buzzer, "vibra": msg.Vibra,
	})
}

// handleGpsGate records the node's GPS power-gate state and reports it.
// While the gate is on, the broker feeds the node RMC+GGA sentences
// synthesized from its slot position (see nmea.go) on the simulation clock,
// which is what a powered GNSS module on the UART would do.
func (ec *extConn) handleGpsGate(msg *emuInbound) {
	s := ec.broker.sim
	s.mu.Lock()
	ec.gpsGen++ // invalidate any feed scheduled under the previous gate state
	ec.gpsOn = msg.On
	if msg.On {
		s.scheduleNMEAFeed(ec, ec.gpsGen)
	}
	s.mu.Unlock()
	s.emitJSON(map[string]interface{}{
		"type": "device_gpsgate", "node": ec.label(), "on": msg.On,
	})
}

// scheduleNMEAFeed schedules the next sentence pair for a gated-on node.
// Must be called under s.mu. The action re-schedules itself for as long as
// the gate stays on and the generation matches (fireBrokerActions runs
// actions under s.mu, so the checks and the re-schedule are race-free). The
// chain ends when the node sends gpsgate off (both set under s.mu in
// handleGpsGate) or when the connection closes (close() clears gpsOn and
// bumps gpsGen under s.mu), so a reset or crashed node cannot leave an
// immortal feed running.
func (s *Sim) scheduleNMEAFeed(ec *extConn, gen uint64) {
	s.scheduleBrokerAction(s.simTime+nmeaFeedIntervalUs, func() {
		if !ec.gpsOn || ec.gpsGen != gen {
			return
		}
		var x, y float32
		if ec.slot != nil {
			x, y = ec.slot.x, ec.slot.y
		}
		ec.sendJSON(nmeaMsg{T: "nmea", Sentence: nmeaRMC(x, y, s.simTime)})
		ec.sendJSON(nmeaMsg{T: "nmea", Sentence: nmeaGGA(x, y, s.simTime)})
		s.scheduleNMEAFeed(ec, gen)
	})
}

// handleLog forwards a firmware console line as this node's console stream.
func (ec *extConn) handleLog(msg *emuInbound) {
	ec.broker.sim.emitConsole(ec.label(), msg.Line)
}

// sendJSON marshals v and enqueues it as one line on the send channel. Drops
// the message if the channel is full (a wedged node must not stall the broker)
// or the connection is closed.
func (ec *extConn) sendJSON(v interface{}) {
	data, err := json.Marshal(v)
	if err != nil {
		return
	}
	data = append(data, '\n')
	select {
	case <-ec.closed:
	case ec.send <- data:
	default:
	}
}

// sendButton delivers a face-button edge (up/down/select/reset) to the node.
// The frontend and gateway (Task 9) drive these; a "reset" edge is how the UI
// reboots a node (the firmware exits, the supervisor restarts it).
func (ec *extConn) sendButton(id, edge string) { ec.sendJSON(btnMsg{T: "btn", ID: id, Edge: edge}) }

// close tears the connection down once, dropping it from the broker registry
// and the sim's external-node map, and marking its slot free for a reconnect.
func (ec *extConn) close() {
	ec.closeOnce.Do(func() {
		close(ec.closed)
		ec.conn.Close()
		b := ec.broker
		s := b.sim

		s.mu.Lock()
		// End any in-flight NMEA feed: a node whose GPS gate was on when it
		// reset or crashed sends no gate-off message, so without this the
		// self-rescheduling feed action would reschedule forever (and every
		// such reset would add another immortal chain to pendingBrokerActions).
		ec.gpsOn = false
		ec.gpsGen++
		if ec.addr != 0 && s.extConns[ec.addr] == ec {
			delete(s.extConns, ec.addr)
			if node := C.node_array_find_by_addr(&s.nodes, C.uint32_t(ec.addr)); node != nil {
				// Keep the sim_node entry (its slot may reconnect) but take it off
				// the air so it stops receiving while detached.
				node.active = C.bool(false)
			}
		}
		s.mu.Unlock()

		b.mu.Lock()
		delete(b.conns, ec)
		if ec.slot != nil && ec.slot.conn == ec {
			ec.slot.conn = nil
		}
		b.mu.Unlock()
	})
}

// --- Sim-side integration (all called under s.mu) ---

// deliverToExternalIfTarget handles an EVT_RECEIVE_PACKET whose destination is
// an attached external node: it runs the collision model exactly as the C
// receive path does and, on a survivable outcome, forwards the frame out over
// emu-link as an rx message. Returns true when it owned the delivery. Harness
// (sim_node firmware) receivers return false and fall through to the C path.
func (s *Sim) deliverToExternalIfTarget(evt *C.sim_event_t) bool {
	if len(s.extConns) == 0 {
		return false
	}
	pkt := C.bridge_get_packet_event(evt)
	ec := s.extConns[uint32(pkt.dest_addr)]
	if ec == nil {
		return false
	}
	rx := C.node_array_find_by_addr(&s.nodes, pkt.dest_addr)
	if rx == nil {
		return true
	}
	outcome := int(C.radio_check_reception(&s.radio, rx, &pkt))
	if outcome == rxOutcomeCollision || outcome == rxOutcomeHalfDuplex {
		return true // audible but destroyed: the node hears nothing
	}
	n := int(pkt.len)
	if n > 256 {
		n = 256
	}
	payload := make([]byte, n)
	for i := 0; i < n; i++ {
		payload[i] = byte(pkt.data[i])
	}
	ec.sendJSON(rxMsg{
		T:       "rx",
		Payload: base64.StdEncoding.EncodeToString(payload),
		RSSI:    int(pkt.rssi),
		SNR:     int(pkt.snr),
		Freq:    s.emuFreq,
	})
	// Deterministic PHY-delivery event: this external node's radio actually
	// received a frame (survived the collision/capture model). Headless scenarios
	// assert channel delivery on this without decoding the opaque payload.
	s.emitJSON(map[string]interface{}{
		"type": "emu_rx", "node": ec.label(), "addr": fmt.Sprintf("0x%08X", ec.addr),
		"len": n, "rssi": int(pkt.rssi),
	})
	return true
}

// channelBusyFor reports whether any node other than addr is currently
// mid-transmission and audible at addr's position. Must be called under s.mu.
func (s *Sim) channelBusyFor(addr uint32) bool {
	self := C.node_array_find_by_addr(&s.nodes, C.uint32_t(addr))
	if self == nil {
		return false
	}
	rng := float32(s.radio._range)
	count := nodeCount(&s.nodes)
	for i := 0; i < count; i++ {
		other := C.node_array_get(&s.nodes, C.int(i))
		if other == nil || uint32(other.addr) == addr || !bool(other.active) {
			continue
		}
		if uint64(other.tx_busy_until_us) <= s.simTime {
			continue
		}
		dx := float32(other.x) - float32(self.x)
		dy := float32(other.y) - float32(self.y)
		if dx*dx+dy*dy <= rng*rng {
			return true
		}
	}
	return false
}

// emitConsole forwards one firmware console line to UI subscribers (and, in
// headless mode, to stdout) tagged with the originating node.
func (s *Sim) emitConsole(node, line string) {
	s.emitJSON(map[string]interface{}{
		"type": "console", "node": node, "line": line,
	})
}

// resetEmulatorForReload tears down per-scenario emulator state at the start of
// a (re)load so a new scenario does not inherit the previous one's firmware
// processes, real-time mode, or external-node routing. Called under s.mu. It
// stops the supervisor (which never takes s.mu, so this is deadlock-safe) but
// leaves the broker listener up for reuse; the broker's slot list is cleared so
// fresh firmware nodes bind fresh slots. Any still-connected external node from
// the prior scenario is orphaned (its C node entry was reset by cmdLoad) and
// drops out when its connection next closes.
func (s *Sim) resetEmulatorForReload() {
	if s.supervisor != nil {
		s.supervisor.Stop()
		s.supervisor = nil
	}
	s.realtime = false
	s.pendingBrokerActions = nil
	s.extConns = make(map[uint32]*extConn)
	s.emuFreq = 0
	s.emuPHYAdopted = false
	s.emuPHYSF = 0
	s.emuPHYBWHz = 0
	if s.broker != nil {
		s.broker.resetSlots()
	}
}

// startEmulator brings up the broker (once) and a fresh supervisor for the
// scenario's firmware nodes, and flips the sim into real-time mode. Called from
// cmdLoad under s.mu.
func (s *Sim) startEmulator(fwNodes []firmwareNodeSpec) {
	s.realtime = true
	if s.broker == nil {
		path := s.emuListen
		if path == "" {
			path = defaultEmuSocketPath()
		}
		b, err := NewBroker(s, path)
		if err != nil {
			log.Printf("emu-link: %v", err)
			s.realtime = false
			return
		}
		s.broker = b
		b.Start()
		log.Printf("emu-link: broker listening on %s", path)
	}
	if s.supervisor != nil {
		s.supervisor.Stop()
	}
	if len(fwNodes) > 0 {
		s.supervisor = NewSupervisor(s.broker, fwNodes)
		s.supervisor.Start()
	}
}

// runRealtimeHeadless runs a firmware scenario headless on the wall clock:
// external node processes boot, attach, beacon, and respond in real time while
// the event loop advances against time.Now, until the scenario duration
// elapses. Returns after tearing the supervisor and broker down.
func (s *Sim) runRealtimeHeadless() error {
	// Reap child firmware nodes on SIGINT/SIGTERM. Without this, gosim (which has
	// no other signal handling) dies immediately on the SIGTERM that `timeout`
	// sends at a budget expiry, or that run_scenarios.sh's cleanup sends, and its
	// node processes are orphaned. Orphans keep burning CPU and contaminate later
	// runs' real-time timing (a real flake source). Closing stopCh routes through
	// the same shutdownEmulator path as normal completion, which kills every node.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	defer signal.Stop(sigCh)
	var stopOnce sync.Once
	go func() {
		if _, ok := <-sigCh; !ok {
			return
		}
		stopOnce.Do(func() { close(s.stopCh) })
	}()

	// Optional wall-clock cap override for constrained CI. The scenario JSON's
	// duration_ms is a fine local default, but a CPU-limited runner pod stretches
	// the real-time render pipeline (message-idle -> auto-open Messages -> e-paper
	// paint) well past it, so gosim would tear the nodes down before the paint
	// lands. The emulator suite (emulator/ci/run_scenarios.sh) sets
	// EMU_SCENARIO_DURATION_MS to widen the cap and then polls the log for the
	// render marker, stopping early once it appears; unset keeps the scenario's
	// own duration so a direct gosim run is unchanged.
	if v := os.Getenv("EMU_SCENARIO_DURATION_MS"); v != "" {
		if ms, err := strconv.ParseUint(v, 10, 64); err == nil && ms > 0 {
			s.mu.Lock()
			s.duration = ms * 1000
			s.mu.Unlock()
		}
	}

	s.mu.Lock()
	s.cmdPlay()
	s.mu.Unlock()

	ticker := time.NewTicker(time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-s.stopCh:
			s.shutdownEmulator()
			return nil
		case <-ticker.C:
		}
		s.mu.Lock()
		if s.state == StateRunning {
			s.advanceSim()
		}
		done := s.state == StateCompleted
		s.mu.Unlock()
		if done {
			break
		}
	}

	s.shutdownEmulator()

	// Flush the C-stdout pipe, mirroring RunHeadless's teardown.
	s.pipeW.Close()
	dup2Stdout(s.origStdout)
	time.Sleep(100 * time.Millisecond)
	return nil
}

func (s *Sim) shutdownEmulator() {
	s.mu.Lock()
	sup := s.supervisor
	br := s.broker
	s.supervisor = nil
	s.broker = nil
	s.mu.Unlock()
	if sup != nil {
		sup.Stop()
	}
	if br != nil {
		br.Stop()
	}
}

// defaultEmuSocketPath returns a per-process default emu-link socket path used
// when a firmware scenario is loaded without an explicit --emu-listen.
func defaultEmuSocketPath() string {
	return fmt.Sprintf("%s/bramble-emu-%d.sock", os.TempDir(), os.Getpid())
}

func firstNonEmpty(a, b string) string {
	if a != "" {
		return a
	}
	return b
}
