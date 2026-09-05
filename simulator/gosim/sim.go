package main

/*
#include "bridge.h"
#include <unistd.h>
#include <stdlib.h>
*/
import "C"
import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"os"
	"slices"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"bramble-sim/websocket"
)

// SimState represents the simulation state machine.
type SimState int

const (
	StateIdle SimState = iota
	StateLoaded
	StateRunning
	StatePaused
	StateCompleted
)

// Command is a message from the WebSocket handler (or CLI) to the sim goroutine.
type Command struct {
	Type          string  `json:"type"`
	Scenario      string  `json:"scenario,omitempty"`
	Value         float64 `json:"value,omitempty"`
	NodeID        string  `json:"node_id,omitempty"`
	X             float32 `json:"x,omitempty"`
	Y             float32 `json:"y,omitempty"`
	Src           string  `json:"src,omitempty"`
	Dest          string  `json:"dest,omitempty"`
	Radius        float32 `json:"radius,omitempty"`
	TelemetryMode string  `json:"telemetry_mode,omitempty"`
	// Node/BtnID/Edge: a face-button edge for an external firmware node's
	// device card (see extnode.go's sendButton).
	// Node is the emu-link hello id (matches node_joined's "node" field, NOT
	// NodeID's simulated-node address space used by add/remove/move_node);
	// BtnID is "up"|"down"|"select"|"reset"; Edge is "down"|"up".
	Node  string `json:"node,omitempty"`
	BtnID string `json:"id,omitempty"`
	Edge  string `json:"edge,omitempty"`
	// Key/Text/To: the emulator control path (emulator/node/emu_control.c).
	// "prov" provisions Key (64 hex chars) on Node, or on every attached
	// firmware node when Node is empty; "send" makes Node originate Text,
	// as a DM when To names a peer address and a channel broadcast when it
	// does not; "attest" makes Node announce its identity now and carries no
	// fields of its own. Node is the emu-link hello id, like the button
	// fields above.
	Key  string `json:"key,omitempty"`
	Text string `json:"text,omitempty"`
	To   string `json:"to,omitempty"`
}

// Sim is the core simulation engine.
type Sim struct {
	mu    sync.RWMutex
	state SimState

	// C state
	nodes    C.node_array_t
	radio    C.radio_config_t
	events   C.event_queue_t
	rng      C.pcg32_state_t
	metrics  C.metrics_state_t
	anomaly  [C.MAX_NODES]C.node_anomaly_tracker_t
	msgTrack [C.MAX_MSG_TRACK]C.msg_tracker_t
	beacon   C.sim_beacon_policy_t // scenario-wide beacon interval policy

	// Sim clock
	simTime    uint64
	duration   uint64
	speed      float64
	wallStart  time.Time
	simAtStart uint64

	// Pipe for capturing C stdout output
	pipeR      *os.File
	pipeW      *os.File
	origStdout int           // saved original stdout fd
	pipeDone   chan struct{} // closed when readPipe returns; nil until started

	nextAddr uint32

	// lastFB / recentConsole hold the most recent device_fb event and the tail
	// of the console stream per node id, guarded by fbMu (a lock of its own,
	// so these hot paths never contend on s.mu). They are what SnapshotEvents
	// replays to a client that connects after the fleet already booted: a node
	// only emits a frame when its screen changes (an e-paper pager can go a
	// minute between frames), and a console line is a one-shot broadcast that
	// a later client would otherwise never see, so both would be missing from
	// a fresh browser's view of a fleet that has been running for a while.
	fbMu          sync.Mutex
	lastFB        map[string][]byte
	recentConsole map[string][][]byte

	cmdCh                  chan Command
	stopCh                 chan struct{}
	broadcast              func([]byte)
	scenarioDir            string
	lastScenario           string
	headless               bool
	broadcastTelemetryMode string

	// "routing" scenario field: "reactive" (default, Bramble's real firmware
	// AODV path via bridge_handle_*) or "flood" (Go-only managed-flooding
	// mode, see flood.go). flood is nil in reactive mode.
	routingMode string
	flood       *floodSim

	// Emulator: external full-firmware nodes attached over the emu-link
	// protocol (extnode.go). realtime is set true whenever the loaded
	// scenario declares firmware nodes or --emu-listen is given; it gates
	// the wall-clock headless loop (runRealtimeHeadless). Pure harness
	// scenarios leave all of these zero/nil and keep the plain virtual-time
	// drain path. extConns maps a live external node's radio address to its
	// connection so EVT_RECEIVE_PACKET delivery can be routed out to the
	// node process instead of into the C firmware. Every field
	// here is read/written only under s.mu, exactly like the C state above.
	realtime bool
	// emuListen is the emu-link unix socket path (from the --emu-listen CLI
	// flag). When non-empty, the broker is started for every loaded scenario so
	// external firmware nodes can attach even if the scenario itself declares
	// none; empty means the broker starts only for scenarios with firmware nodes.
	emuListen string
	// disableCollisions forces the collision/half-duplex model off for every
	// loaded scenario (from the --no-collisions CLI flag).
	disableCollisions    bool
	broker               *Broker
	supervisor           *Supervisor
	extConns             map[uint32]*extConn
	pendingBrokerActions []brokerAction
	// emuFreq is the ether's single-channel carrier (Hz), learned from the
	// most recent tx's freq and echoed on every rx. The model is
	// single-channel, so a received frame's frequency is the channel's.
	emuFreq int
	// emuPHYPinned is true when the scenario's "radio" block declared sf or
	// bw_hz. Pinned scenarios keep the author's PHY; unpinned ones adopt the
	// PHY an attached firmware node reports (see extConn.adoptReportedPHY).
	emuPHYPinned bool
	// emuPHYAdopted records the (sf, bw_hz) already learned from a firmware
	// node, so the adoption happens once and a second node disagreeing about
	// the single-channel ether's PHY is reported instead of silently winning.
	emuPHYAdopted bool
	emuPHYSF      int
	emuPHYBWHz    int
}

// brokerAction is a deferred broker-side side effect (e.g. sending txdone
// after a transmission's deterministic time-on-air elapses) scheduled on the
// simulation clock. It fires in fireBrokerActions when simTime reaches dueUs,
// so the timing rides the same clock the event loop uses (wall clock in
// real-time mode) while the airtime VALUE that set dueUs stays deterministic.
type brokerAction struct {
	dueUs uint64
	fn    func()
}

// NewSim creates a new simulation engine. emuListen and disableCollisions are
// the two process-wide CLI settings (--emu-listen, --no-collisions); pass ""
// and false for the defaults used outside the server and headless entry points.
func NewSim(scenarioDir string, broadcast func([]byte), headless bool, emuListen string, disableCollisions bool) (*Sim, error) {
	s := &Sim{
		state:                  StateIdle,
		speed:                  1.0,
		nextAddr:               0x1000,
		cmdCh:                  make(chan Command, 64),
		stopCh:                 make(chan struct{}),
		broadcast:              broadcast,
		scenarioDir:            scenarioDir,
		headless:               headless,
		broadcastTelemetryMode: "full",
		emuListen:              emuListen,
		disableCollisions:      disableCollisions,
		extConns:               make(map[uint32]*extConn),
		lastFB:                 make(map[string][]byte),
		recentConsole:          make(map[string][][]byte),
	}

	// Firmware-default beacon policy until a scenario overrides it (cmdLoad)
	C.sim_beacon_policy_init(&s.beacon)

	// Create pipe to capture C stdout output
	r, w, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("os.Pipe: %w", err)
	}
	s.pipeR = r
	s.pipeW = w

	// Save original stdout and redirect fd 1 to pipe write end
	s.origStdout, err = syscall.Dup(1)
	if err != nil {
		r.Close()
		w.Close()
		return nil, fmt.Errorf("dup stdout: %w", err)
	}
	if err := syscall.Dup2(int(w.Fd()), 1); err != nil {
		syscall.Close(s.origStdout)
		r.Close()
		w.Close()
		return nil, fmt.Errorf("dup2: %w", err)
	}

	// Bridge-level state, initialized only once fd 1 points at the capture
	// pipe: bridge_init emits a public_channel_init event of its own, and if
	// it ran before that redirect, that one line would go straight to the
	// process's real stdout, reaching neither a WebSocket client nor a
	// captured run.
	C.bridge_init()

	return s, nil
}

// Start launches the simulation goroutines.
func (s *Sim) Start() {
	s.startPipeReader()
	go s.run()
}

// Send sends a command to the simulation.
func (s *Sim) Send(cmd Command) {
	s.cmdCh <- cmd
}

// State returns the current simulation state.
func (s *Sim) State() SimState {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.state
}

// Stop shuts down the simulation.
func (s *Sim) Stop() {
	close(s.stopCh)
	// Restore stdout; pipeR then gets EOF and readPipe exits.
	s.restoreStdout()
}

// tickAdvance steps the sim one tick under the sim lock, advancing only while
// it is running, and reports whether the sim has reached the completed state.
// Both real-time drivers (run and runRealtimeHeadless) step through this so
// their lock-and-advance discipline stays identical.
func (s *Sim) tickAdvance() (completed bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.state == StateRunning {
		s.advanceSim()
	}
	return s.state == StateCompleted
}

// run is the main simulation goroutine.
func (s *Sim) run() {
	ticker := time.NewTicker(time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-s.stopCh:
			return
		case cmd := <-s.cmdCh:
			s.handleCommand(cmd)
		case <-ticker.C:
			s.tickAdvance()
		}
	}
}

func (s *Sim) handleCommand(cmd Command) {
	s.mu.Lock()
	defer s.mu.Unlock()

	switch cmd.Type {
	case "load", "start":
		s.cmdLoad(cmd)
		// Firmware/external nodes are real-time processes: they boot and
		// transmit on the wall clock the moment they attach, and "pause" has
		// no meaning for them. Delivery only runs while StateRunning, so a
		// firmware scenario left in StateLoaded silently drops every real-time
		// transmission until a human presses Play, with no UI cue that Play is
		// required (this read as "devices show but messages never get sent").
		// Auto-start on the interactive load path so a loaded firmware
		// scenario just works. This is deliberately NOT inside cmdLoad, which
		// RunHeadless also calls and which contractually must leave the sim in
		// StateLoaded; the headless path does its own state management. Pure
		// virtual-time scenarios leave s.realtime false and still start paused.
		if s.realtime {
			s.cmdPlay()
		}
	case "play":
		s.cmdPlay()
	case "pause":
		s.cmdPause()
	case "restart":
		s.cmdRestart()
	case "speed":
		s.cmdSpeed(cmd)
	case "instant":
		s.cmdInstant()
	case "add_node":
		s.cmdAddNode(cmd)
	case "remove_node":
		s.cmdRemoveNode(cmd)
	case "move_node":
		s.cmdMoveNode(cmd)
	case "send_message":
		s.cmdSendMessage(cmd)
	case "interference":
		s.cmdInterference(cmd)
	case "set_broadcast_telemetry_mode":
		s.cmdSetBroadcastTelemetryMode(cmd)
	case "btn":
		s.cmdButton(cmd)
	case "prov":
		s.cmdProvision(cmd)
	case "send":
		s.cmdSend(cmd)
	case "attest":
		s.cmdAttest(cmd)
	default:
		log.Printf("unknown command: %s", cmd.Type)
	}
}

// advanceSim processes events up to the current sim time.
func (s *Sim) advanceSim() {
	elapsed := time.Since(s.wallStart)
	simNow := s.simAtStart + uint64(float64(elapsed.Microseconds())*s.speed)

	// Cap at duration
	if s.duration > 0 && simNow >= s.duration {
		simNow = s.duration
	}

	s.pump(simNow)

	// Check if simulation complete
	if s.duration > 0 && simNow >= s.duration {
		s.complete()
	}
}

// pump advances the simulation clock to simNow, dispatches every C event due
// at or before it, then fires any broker-side deferred actions that have
// come due. Split out of advanceSim so both the wall-clock loops
// (advanceSim, runRealtimeHeadless) and the emulator tests drive events the
// same way. Non-realtime scenarios never schedule broker actions, so the
// fireBrokerActions call is a no-op for them and their behavior is unchanged.
func (s *Sim) pump(simNow uint64) {
	s.simTime = simNow
	setSimTime(simNow)

	var evt C.sim_event_t
	for {
		peek := eventQueuePeek(&s.events)
		if peek == nil {
			break
		}
		ts := getEventTimestamp(peek)
		if ts > simNow {
			break
		}
		if !eventQueuePop(&s.events, &evt) {
			break
		}
		s.dispatchEvent(&evt)
	}

	s.fireBrokerActions(simNow)
}

// fireBrokerActions runs every scheduled broker action whose due time has been
// reached, in due-time order, and drops it from the pending list. Called under
// s.mu (via pump); the actions themselves only enqueue bytes onto a
// connection's buffered send channel, so they never block on I/O while the
// lock is held.
func (s *Sim) fireBrokerActions(simNow uint64) {
	if len(s.pendingBrokerActions) == 0 {
		return
	}
	sort.Slice(s.pendingBrokerActions, func(i, j int) bool {
		return s.pendingBrokerActions[i].dueUs < s.pendingBrokerActions[j].dueUs
	})
	kept := s.pendingBrokerActions[:0]
	var due []brokerAction
	for _, a := range s.pendingBrokerActions {
		if a.dueUs <= simNow {
			due = append(due, a)
		} else {
			kept = append(kept, a)
		}
	}
	// kept aliases the backing array; copy the survivors out before firing so a
	// fired action that schedules a new one does not corrupt the slice we are
	// still compacting.
	survivors := make([]brokerAction, len(kept))
	copy(survivors, kept)
	s.pendingBrokerActions = survivors
	for _, a := range due {
		a.fn()
	}
}

// scheduleBrokerAction registers fn to fire once the simulation clock reaches
// dueUs. Must be called under s.mu.
func (s *Sim) scheduleBrokerAction(dueUs uint64, fn func()) {
	s.pendingBrokerActions = append(s.pendingBrokerActions, brokerAction{dueUs: dueUs, fn: fn})
}

func (s *Sim) dispatchEvent(evt *C.sim_event_t) {
	evtType := getEventType(evt)

	switch evtType {
	case C.EVT_TICK_NODE:
		s.handleTickNode(evt)
	case C.EVT_RECEIVE_PACKET:
		// A frame addressed to (or, being PHY-broadcast, audible at) an
		// external firmware node is delivered out over emu-link instead of into
		// the C firmware. deliverToExternalIfTarget returns true when it owned
		// the delivery; only harness (sim_node) receivers fall through to the C
		// path below, so a pure-harness scenario (no firmware nodes) always
		// dispatches through the C path here.
		if s.deliverToExternalIfTarget(evt) {
			return
		}
		if s.routingMode == "flood" {
			s.handleReceivePacketFlood(evt)
		} else {
			s.handleReceivePacket(evt)
		}
	case C.EVT_GENERATE_MESSAGE:
		if s.routingMode == "flood" {
			s.handleGenerateMessageFlood(evt)
		} else {
			s.handleGenerateMessage(evt)
		}
	case C.EVT_SEND_PACKET:
		if s.routingMode == "flood" {
			s.handleFloodRelayDue(evt)
		} else {
			s.handleChannelFloodRelay(evt)
		}
	case C.EVT_NODE_JOIN:
		s.handleNodeJoin(evt)
	case C.EVT_NODE_LEAVE:
		s.handleNodeLeave(evt)
	case C.EVT_NODE_MOVE:
		s.handleNodeMove(evt)
	case C.EVT_INTERFERENCE_START:
		s.handleInterferenceStart(evt)
	case C.EVT_INTERFERENCE_END:
		s.handleInterferenceEnd(evt)
	case C.EVT_METRICS_TICK:
		s.handleMetricsTick(evt)
	case C.EVT_GENERATE_ATTESTATION:
		// Attestations always go through the real firmware C path in
		// bridge.c (there is no Go-model flood equivalent; scenarios that
		// script send_attestation use the default routing mode).
		s.handleGenerateAttestation(evt)
	case C.EVT_PROVISION_ANCHOR:
		// Runtime setAnchor equivalent; (re-)anchors a node and drops any
		// stale un-endorsed pins.
		s.handleProvisionAnchor(evt)
	case C.EVT_RECEIPT_TX:
		// One queued broadcast delivery receipt has come due on one node
		// (the sim's MESH_EVT_RECEIPT_TX).
		s.handleReceiptTx(evt)
	case C.EVT_GENERATE_LOCATION:
		// Position broadcasts always go through the real firmware C path in
		// bridge.c, same rationale as attestations above.
		s.handleGenerateLocation(evt)
	case C.EVT_GENERATE_ROLLCALL:
		// Attested roll-call: a scripted initiation, driven through the
		// real components/rollcall core in bridge.c (same rationale as
		// attestations above: there is no Go-model equivalent).
		s.handleGenerateRollCall(evt)
	case C.EVT_ROLLCALL_ROUND:
		// One re-announce round, or the close sweep after the last one.
		s.handleRollCallRound(evt)
	case C.EVT_ROLLCALL_TX:
		// One member's staggered, identity-signed answer has come due.
		s.handleRollCallTx(evt)
	}
}

// --- Event handlers ---

func (s *Sim) handleTickNode(evt *C.sim_event_t) {
	tickData := C.bridge_get_tick_event(evt)
	nodeID := C.GoString(&tickData.node_id[0])

	node := nodeArrayFindByID(&s.nodes, nodeID)
	if node == nil || !bool(node.active) {
		return
	}

	// Managed flooding has no periodic control-plane duty (no beacons --
	// there is no neighbor table for one to serve -- no route maintenance,
	// no per-hop retransmit ladder). Nothing to do on a tick, and nothing
	// depends on rescheduling it: flood mode's own
	// EVT_RECEIVE_PACKET/EVT_SEND_PACKET handlers (flood.go) drive
	// everything else.
	if s.routingMode == "flood" {
		return
	}

	var result C.node_tick_result_t
	ts := getEventTimestamp(evt)
	C.node_tick(node, C.uint64_t(ts), &s.radio, &s.beacon, &result)

	// Broadcast any outbound packets
	for i := 0; i < int(result.count); i++ {
		C.sim_radio_broadcast(node, &result.pkts[i],
			&s.nodes, &s.radio, &s.rng, &s.events, &s.metrics, C.uint64_t(ts))
	}

	// Check for retransmissions
	C.bridge_handle_retransmit(node, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics, C.uint64_t(ts))

	// Reschedule next tick (1 second later)
	nextTick := C.bridge_make_tick_event(C.uint64_t(ts+1000000), &tickData.node_id[0], tickData.tick_seq+1)
	eventQueuePush(&s.events, &nextTick)
}

func (s *Sim) handleReceivePacket(evt *C.sim_event_t) {
	C.bridge_handle_receive_packet(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.anomaly[0], &s.msgTrack[0], C.MAX_MSG_TRACK)
}

func (s *Sim) handleGenerateMessage(evt *C.sim_event_t) {
	C.bridge_handle_generate_message(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.anomaly[0], &s.msgTrack[0], C.MAX_MSG_TRACK)
}

// handleGenerateAttestation fires a scripted identity-attestation origination
// (the "send_attestation" scenario event): the named node signs and
// broadcasts its (or, for the impersonation scenario, someone else's)
// address binding through the real firmware origination path in bridge.c.
func (s *Sim) handleGenerateAttestation(evt *C.sim_event_t) {
	C.bridge_handle_generate_attestation(evt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics)
}

// handleGenerateLocation: a scripted GPS position broadcast through the
// real firmware location serialization path in bridge.c. Every
// in-range receiver caches it via the real location_cache_update.
func (s *Sim) handleGenerateLocation(evt *C.sim_event_t) {
	C.bridge_handle_generate_location(evt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics)
}

// handleGenerateRollCall starts a scripted attested roll-call: the real
// initiation rate limit, the real ledger, and round 1 flooded as an ordinary
// broadcast DATA frame on the BROADCAST airtime lane.
func (s *Sim) handleGenerateRollCall(evt *C.sim_event_t) {
	C.bridge_handle_generate_rollcall(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.msgTrack[0], C.MAX_MSG_TRACK)
}

// handleRollCallRound fires one due re-announce round, or (round 0) the close
// sweep that shuts the ledger and reports who answered.
func (s *Sim) handleRollCallRound(evt *C.sim_event_t) {
	C.bridge_handle_rollcall_round(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.msgTrack[0], C.MAX_MSG_TRACK)
}

// handleRollCallTx fires one member's staggered answer: signed with that
// node's real Ed25519 identity key and unicast home on the NORMAL lane,
// waiting behind ordinary reactive route discovery when the member holds no
// route to the initiator.
func (s *Sim) handleRollCallTx(evt *C.sim_event_t) {
	C.bridge_handle_rollcall_tx(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.anomaly[0], &s.msgTrack[0], C.MAX_MSG_TRACK)
}

// handleProvisionAnchor: a scripted runtime setAnchor. Re-anchors the node to
// the fleet test anchor via the real identity_store_set_anchor, dropping any
// stale pins it held while un-anchored.
func (s *Sim) handleProvisionAnchor(evt *C.sim_event_t) {
	C.bridge_handle_provision_anchor(evt, &s.nodes)
}

// handleReceiptTx fires one due broadcast delivery receipt through bridge.c's
// mirror of firmware's mesh_process_receipt_tx_event: the real airtime gate,
// the real LBT, and the per-kind defer-or-blind-fire decision.
func (s *Sim) handleReceiptTx(evt *C.sim_event_t) {
	C.bridge_handle_receipt_tx(evt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics)
}

// handleChannelFloodRelay fires one jittered relay that bridge.c's shared
// firmware flood engine (bridge_flood_relay, on top of channel_flood_decide)
// scheduled as an EVT_SEND_PACKET. Broadcast DATA, unicast DATA under flood
// transport, delivery receipts, and identity attestations all schedule through
// that one engine. Distinct from handleFloodRelayDue, which rebroadcasts
// through the Go-only floodSim model rather than the firmware channel flood.
func (s *Sim) handleChannelFloodRelay(evt *C.sim_event_t) {
	C.bridge_handle_flood_relay(evt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics)
}

// applyDutyCycleCap re-applies the scenario's optional regulatory
// duty-cycle cap to a node's real airtime budget via the real
// airtime_budget_set_duty_cap. Must run after every node_activate, since
// node_activate's airtime_budget_init resets the cap; no-op if the
// scenario's "radio" block has no duty_cycle_pct (unlimited).
func (s *Sim) applyDutyCycleCap(node *C.sim_node_t) {
	if bool(s.radio.duty_cycle_set) {
		C.bridge_apply_duty_cycle_cap(node, s.radio.duty_cycle_pct)
	}
}

func (s *Sim) handleNodeJoin(evt *C.sim_event_t) {
	nd := C.bridge_get_node_event(evt)
	nodeID := C.GoString(&nd.node_id[0])
	ts := getEventTimestamp(evt)

	// A rejoin must REUSE the existing entry, like firmware keeps its NVS
	// identity across a reboot. Appending a duplicate would leave two
	// entries with the same id and address, with every find-by-id lookup
	// resolving to the deactivated corpse while the live node sits stranded
	// at (0,0). nodeActivate below models the reboot: volatile protocol
	// state (routes, neighbors, pending acks) is cleared, the persistent
	// identity survives.
	idx := -1
	for i := 0; i < int(s.nodes.count); i++ {
		if C.GoString(&s.nodes.nodes[i].id[0]) == nodeID {
			idx = i
			break
		}
	}
	var node *C.sim_node_t
	if idx >= 0 {
		node = C.node_array_get(&s.nodes, C.int(idx))
		// Explicit event coordinates win over the node's remembered
		// position; a coordinate-less rejoin restores the node's original
		// scenario position instead of teleporting it to (0,0).
		if bool(nd.has_coords) {
			node.x = nd.x
			node.y = nd.y
		} else {
			node.x = node.home_x
			node.y = node.home_y
		}
	} else {
		idx = nodeArrayAdd(&s.nodes, nodeID, uint32(nd.addr), float32(nd.x), float32(nd.y))
		if idx < 0 {
			log.Printf("failed to add node %s", nodeID)
			return
		}
		node = C.node_array_get(&s.nodes, C.int(idx))
	}
	nodeActivate(node)
	s.applyDutyCycleCap(node)
	anomalyInit(&s.anomaly[idx])

	// Initialize extended node state (mailbox, location, etc.). node.addr,
	// not nd.addr: node_array_add derives the address from the node's
	// Ed25519 identity key. node.x/node.y, not nd.x/nd.y: the resolved
	// position (event coords, or the restored original on a coordinate-less
	// rejoin).
	C.bridge_handle_node_join_ext(C.int(idx), node.addr,
		node.x, node.y, C.uint64_t(ts))

	// Schedule first tick
	cid := C.CString(nodeID)
	tick := C.bridge_make_tick_event(C.uint64_t(ts+100000), cid, 0)
	C.free(unsafe.Pointer(cid))
	eventQueuePush(&s.events, &tick)

	s.emitJSON(map[string]any{
		"type": "node_joined", "timestamp_us": ts,
		"node": nodeID, "addr": fmt.Sprintf("0x%08X", uint32(node.addr)),
		"x": node.x, "y": node.y,
	})
}

func (s *Sim) handleNodeLeave(evt *C.sim_event_t) {
	nd := C.bridge_get_node_event(evt)
	nodeID := C.GoString(&nd.node_id[0])
	ts := getEventTimestamp(evt)

	node := nodeArrayFindByID(&s.nodes, nodeID)
	if node != nil {
		nodeDeactivate(node)
	}

	s.emitJSON(map[string]any{
		"type": "node_left", "timestamp_us": ts, "node": nodeID,
	})

	anomalyCheckPartition(&s.nodes, &s.radio, ts)
}

func (s *Sim) handleNodeMove(evt *C.sim_event_t) {
	nd := C.bridge_get_node_event(evt)
	nodeID := C.GoString(&nd.node_id[0])
	ts := getEventTimestamp(evt)

	node := nodeArrayFindByID(&s.nodes, nodeID)
	if node != nil {
		nodeMove(node, float32(nd.x), float32(nd.y))
	}

	s.emitJSON(map[string]any{
		"type": "node_moved", "timestamp_us": ts,
		"node": nodeID, "x": nd.x, "y": nd.y,
	})
}

func (s *Sim) handleInterferenceStart(evt *C.sim_event_t) {
	idata := C.bridge_get_interference_event(evt)
	C.radio_add_interference_zone(&s.radio, idata.center_x, idata.center_y, idata.radius)
}

func (s *Sim) handleInterferenceEnd(evt *C.sim_event_t) {
	idata := C.bridge_get_interference_event(evt)
	C.radio_clear_interference_zone(&s.radio, idata.zone_index)
}

// putSharedMetrics fills the counter and rate fields the periodic "metrics"
// tick and the terminal "final_metrics" event report identically into m.
// Both events call this instead of inlining these 19 key/value pairs
// verbatim, so a change to one field (a renamed counter, a different
// divisor) cannot land in only one of the two payloads and let them silently
// drift. The fields that legitimately differ between the two events
// (messages_sent/delivered/dropped, which the final event ties to the same
// terminal-state locals its rate math uses) are set by each caller.
func (s *Sim) putSharedMetrics(m map[string]any) {
	m["total_packets"] = uint64(s.metrics.total_packets)
	m["retried"] = uint64(s.metrics.messages_retried)
	m["delivered_on_retry"] = uint64(s.metrics.messages_delivered_retry)
	m["dedup_dropped"] = uint64(s.metrics.dedup_dropped)
	m["airtime_deferred"] = uint64(s.metrics.airtime_deferred)
	m["fragments_sent"] = uint64(s.metrics.fragments_sent)
	m["fragments_reassembled"] = uint64(s.metrics.fragments_reassembled)
	m["reassembly_timeout"] = uint64(s.metrics.reassembly_timeout)
	m["crypto_encrypted"] = uint64(s.metrics.crypto_encrypted)
	m["crypto_decrypted"] = uint64(s.metrics.crypto_decrypted)
	m["crypto_auth_failed"] = uint64(s.metrics.crypto_auth_failed)
	m["collisions"] = uint64(s.metrics.collisions)
	m["half_duplex_drops"] = uint64(s.metrics.half_duplex_drops)
	m["capture_wins"] = uint64(s.metrics.capture_wins)
	m["lbt_backoffs"] = uint64(s.metrics.lbt_backoffs)
	m["receptions_ok"] = uint64(s.metrics.receptions_ok)
	m["channel_log_overflow"] = uint64(s.radio.channel.overflow_drops)
	m["airtime_total_ms"] = uint64(s.metrics.airtime_total_us) / 1000
	m["avg_latency_ms"] = float64(C.metrics_avg_latency_ms(&s.metrics))
}

func (s *Sim) handleMetricsTick(evt *C.sim_event_t) {
	ts := getEventTimestamp(evt)

	// Count active nodes
	active := 0
	count := nodeCount(&s.nodes)
	for i := 0; i < count; i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && bool(node.active) {
			active++
		}
	}
	C.metrics_update_active_nodes(&s.metrics, C.int(active))

	metrics := map[string]any{
		"type":          "metrics",
		"timestamp_us":  ts,
		"active_nodes":  active,
		"messages_sent": uint64(s.metrics.messages_sent),
		"delivered":     uint64(s.metrics.delivered_packets),
		"dropped":       uint64(s.metrics.dropped_packets),
	}
	s.putSharedMetrics(metrics)
	s.emitJSON(metrics)

	// Check black holes
	for i := 0; i < count; i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && bool(node.active) {
			C.anomaly_check_blackhole(&s.anomaly[i].blackhole, C.uint64_t(ts), C.stdout, &node.id[0])
		}
	}
	// Note: metrics ticks are pre-scheduled by scenario_load_file, no rescheduling needed
}

// --- Command handlers ---

func (s *Sim) cmdLoad(cmd Command) {
	scenarioName := cmd.Scenario
	if scenarioName == "" {
		scenarioName = "10-node-grid"
	}
	// Resolve scenario path: if it's just a name, look in scenarioDir
	scenarioPath := scenarioName
	if !strings.Contains(scenarioName, "/") {
		scenarioPath = fmt.Sprintf("%s/%s.json", s.scenarioDir, scenarioName)
	}

	// Tear down any emulator state from a prior load before rebuilding.
	// Safe to call under s.mu (it stops the supervisor, which never takes s.mu,
	// and leaves the broker listener up for reuse).
	s.resetEmulatorForReload()

	// Reset C state
	nodeArrayInit(&s.nodes)
	radioConfigInit(&s.radio)
	eventQueueInit(&s.events)
	C.metrics_init(&s.metrics)
	C.bridge_msg_track_init(&s.msgTrack[0], C.MAX_MSG_TRACK)
	for i := 0; i < C.MAX_NODES; i++ {
		anomalyInit(&s.anomaly[i])
	}

	scenario, ok := loadScenario(scenarioPath, &s.nodes, &s.radio, &s.events, &s.rng)
	if !ok {
		log.Printf("failed to load scenario: %s", scenarioPath)
		return
	}

	// The Go-side scenario extensions below (routing, intermediate_rrep,
	// flood_transport, per-node trust flags, firmware_nodes) each read a few
	// optional fields the C cJSON loader ignores. Read the file once here and
	// hand the bytes to every parser instead of re-reading and re-parsing the
	// same file per field; a read failure leaves scenarioData nil, and each
	// parser falls open to its shipped default.
	scenarioData, _ := os.ReadFile(scenarioPath)

	// Optional "routing"/"flood_hop_limit" fields, read directly off the
	// scenario bytes (see flood.go's loadRoutingConfig), independent of the
	// C-side cJSON parse above. Defaults to "reactive" for every scenario
	// that omits "routing".
	routingMode, floodHopLimit := loadRoutingConfig(scenarioData)
	s.routingMode = routingMode
	if routingMode == "flood" {
		s.flood = newFloodSim(floodHopLimit)
	} else {
		s.flood = nil
	}

	// Optional "intermediate_rrep" scenario field, read the same way as
	// "routing" above (independent Go-side JSON read, so this schema
	// extension needs no C-side sim_scenario changes). Defaults to true
	// (firmware's always-on shipped behavior);
	// explicitly re-applied on every run (not just when disabling) so one
	// scenario's setting never leaks into the next run in the same process
	// (see bridge.h's doc comment on bridge_set_intermediate_rrep_enabled).
	C.bridge_set_intermediate_rrep_enabled(C.bool(loadIntermediateRREPConfig(scenarioData)))

	// Optional "flood_transport" scenario field, read the same way as
	// "intermediate_rrep" above (independent Go-side JSON read, no C-side
	// sim_scenario changes needed). Drives the REAL firmware flood transport
	// through bridge.c (see flood.go's loadFloodTransportConfig doc
	// comment); distinct from s.routingMode's Go-only "flood" MODEL.
	// Defaults to false (firmware's shipped NVS default); re-applied on every
	// load so one scenario's setting never leaks into the next run in the
	// same process.
	floodTransport, floodTransportHopLimit := loadFloodTransportConfig(scenarioData)
	C.bridge_set_flood_transport_enabled(C.bool(floodTransport))
	// Optional "flood_hop_limit" scenario field drives the flood-transport
	// origination hop budget (firmware's s_flood_hop_limit), re-applied on
	// every load. bridge_set_flood_hop_limit clamps to the firmware range; a
	// farther-reaching flood needs a larger value here.
	C.bridge_set_flood_hop_limit(C.uint8_t(floodTransportHopLimit))

	// Optional "receipt_tx_kind" scenario field ("receipt" |
	// "receipt_forward"), read Go-side like the fields above. Selects which
	// real tx_kind_t an ORIGINATED broadcast delivery receipt is transmitted
	// as, which is what decides whether exhausting LBT on a busy channel
	// defers the send or blind-fires into it (see bridge.h). Defaults to the
	// shipped firmware kind; re-applied on every load so no run leaks a
	// previous run's arm.
	C.bridge_set_broadcast_receipt_tx_kind(C.int(loadReceiptTxKindConfig(scenarioData)))

	// Seed the RNG (scenario_load_file only seeds for stochastic mode)
	C.pcg32_seed(&s.rng, scenario.metadata.seed)
	if s.disableCollisions {
		s.radio.collisions_enabled = C.bool(false)
	}
	s.duration = uint64(scenario.metadata.duration_us)
	s.beacon = scenario.beacon
	s.simTime = 0
	s.speed = 1.0
	s.nextAddr = 0x1000
	s.state = StateLoaded

	// Broadcast sim_reset
	s.emitJSON(map[string]any{"type": "sim_reset"})

	// Optional per-node trust-state flags, read Go-side like
	// flood_transport/intermediate_rrep (no C-side sim_scenario change) in a
	// single scenario parse. Each defaults to the fully-provisioned state for
	// every node:
	//   - "unprovisioned": boots without the network key and stays inert;
	//     defaults to provisioned.
	//   - "unendorsed": boots without a fleet-anchor endorsement so anchored
	//     receivers refuse to pin it; defaults to endorsed, so existing
	//     scenarios still pin under the endorsed-only gate.
	//   - "unanchored": boots without a fleet anchor and TOFU-pins until a
	//     provision_anchor event hardens it; defaults to anchored (the
	//     harness default).
	// Trust overrides, applied to each listed node AFTER join (join defaults
	// every node to the trusted state: provisioned, endorsed, and anchored).
	// Each entry flips one trust bit for the nodes the scenario names under its
	// flag key and announces it; see the loadNodeTrustFlags notes above for what
	// each override models.
	trustFlags := loadNodeTrustFlags(scenarioData)
	trustOverrides := []struct {
		nodes     map[string]bool
		mark      func(int)
		eventType string
	}{
		{trustFlags.unprovisioned, nodeMarkUnprovisioned, "node_unprovisioned"},
		{trustFlags.unendorsed, nodeMarkUnendorsed, "node_unendorsed"},
		{trustFlags.unanchored, nodeMarkUnanchored, "node_unanchored"},
	}

	// Broadcast node_joined for each initial node
	count := nodeCount(&s.nodes)
	for i := 0; i < count; i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node == nil {
			continue
		}
		nodeID := C.GoString(&node.id[0])
		nodeActivate(node)
		s.applyDutyCycleCap(node)
		anomalyInit(&s.anomaly[i])

		// Initialize extended node state exactly like a dynamic join does
		// (handleNodeJoin): position, and the node's Ed25519 identity
		// keypair. Skipping this would leave initial scenario nodes without
		// identities and unable to originate or pin attestations.
		C.bridge_handle_node_join_ext(C.int(i), C.uint32_t(node.addr),
			node.x, node.y, C.uint64_t(0))

		for _, ov := range trustOverrides {
			if ov.nodes[nodeID] {
				ov.mark(i)
				s.emitJSON(map[string]any{
					"type": ov.eventType, "timestamp_us": 0,
					"node": nodeID,
				})
			}
		}

		// Schedule initial tick (staggered by 100ms per node)
		tick := C.bridge_make_tick_event(C.uint64_t(uint64(i)*100000), &node.id[0], 0)
		eventQueuePush(&s.events, &tick)

		s.emitJSON(map[string]any{
			"type": "node_joined", "timestamp_us": 0,
			"node": nodeID,
			"addr": fmt.Sprintf("0x%08X", node.addr),
			"x":    node.x, "y": node.y,
		})
	}

	// Metrics ticks need no scheduling here: scenario_load_file pre-schedules them.

	// Broadcast config + sim_ready
	s.emitJSON(map[string]any{
		"type":        "config",
		"radio_range": s.radio._range,
		"duration_us": s.duration,
		"collisions":  bool(s.radio.collisions_enabled),
		"sf":          uint8(s.radio.sf),
		"bw_hz":       uint32(s.radio.bw_hz),
		"cr":          uint8(s.radio.cr),
	})
	s.emitJSON(map[string]any{"type": "sim_ready"})

	// If this scenario declares firmware nodes (or --emu-listen opened the
	// socket), bring up the emu-link broker and the process supervisor and
	// switch the scenario to real-time (wall-clock) execution. Pure harness
	// scenarios declare no firmware nodes and leave s.realtime false, leaving
	// their virtual-time path unaffected.
	fwNodes := loadFirmwareNodes(scenarioData)
	s.emuPHYPinned = scenarioPinsPHY(scenarioData)
	if len(fwNodes) > 0 || s.emuListen != "" {
		s.startEmulator(fwNodes)
	}

	s.lastScenario = scenarioPath
	log.Printf("loaded scenario: %s (%d nodes, duration %d us)", scenarioPath, count, s.duration)
}

func (s *Sim) cmdPlay() {
	if s.state != StateLoaded && s.state != StatePaused {
		return
	}
	s.wallStart = time.Now()
	s.simAtStart = s.simTime
	s.state = StateRunning

	s.emitJSON(map[string]any{
		"type": "sim_state", "state": "running",
	})
}

func (s *Sim) cmdPause() {
	if s.state != StateRunning {
		return
	}
	// Anchor current sim time
	elapsed := time.Since(s.wallStart)
	s.simTime = s.simAtStart + uint64(float64(elapsed.Microseconds())*s.speed)
	s.state = StatePaused

	s.emitJSON(map[string]any{
		"type": "sim_state", "state": "paused",
	})
}

func (s *Sim) cmdRestart() {
	s.cmdLoad(Command{Scenario: s.lastScenario})
}

func (s *Sim) cmdSpeed(cmd Command) {
	if s.state == StateRunning {
		// Re-anchor
		elapsed := time.Since(s.wallStart)
		s.simTime = s.simAtStart + uint64(float64(elapsed.Microseconds())*s.speed)
		s.simAtStart = s.simTime
		s.wallStart = time.Now()
	}
	s.speed = cmd.Value
	if s.speed <= 0 {
		s.speed = 1.0
	}
	s.emitJSON(map[string]any{
		"type": "speed_changed", "speed": s.speed,
	})
}

func (s *Sim) cmdInstant() {
	if s.state != StateLoaded && s.state != StatePaused && s.state != StateRunning {
		return
	}
	s.state = StateRunning

	// Process all remaining events
	var evt C.sim_event_t
	for eventQueuePop(&s.events, &evt) {
		ts := getEventTimestamp(&evt)
		s.simTime = ts
		setSimTime(ts)
		s.dispatchEvent(&evt)
	}

	s.complete()
}

func (s *Sim) cmdAddNode(cmd Command) {
	s.nextAddr++
	addr := s.nextAddr

	// Auto-generate node ID if not provided
	nodeID := cmd.NodeID
	if nodeID == "" {
		nodeID = fmt.Sprintf("N%d", addr)
	}

	// Auto-place near a random existing node if no position given
	x, y := cmd.X, cmd.Y
	if x == 0 && y == 0 && s.nodes.count > 0 {
		pick := int(C.pcg32_random(&s.rng)) % int(s.nodes.count)
		ref := C.node_array_get(&s.nodes, C.int(pick))
		offset := float32(50 + int(C.pcg32_random(&s.rng))%50)
		angle := float32(C.pcg32_random(&s.rng)) / float32(0xFFFFFFFF) * 6.283185
		x = float32(ref.x) + offset*float32(math.Cos(float64(angle)))
		y = float32(ref.y) + offset*float32(math.Sin(float64(angle)))
	}

	idx := nodeArrayAdd(&s.nodes, nodeID, uint32(addr), x, y)
	if idx < 0 {
		log.Printf("failed to add node %s", nodeID)
		return
	}
	node := C.node_array_get(&s.nodes, C.int(idx))
	nodeActivate(node)
	s.applyDutyCycleCap(node)
	anomalyInit(&s.anomaly[idx])

	// Schedule tick
	cid := C.CString(nodeID)
	tick := C.bridge_make_tick_event(C.uint64_t(s.simTime+100000), cid, 0)
	C.free(unsafe.Pointer(cid))
	eventQueuePush(&s.events, &tick)

	s.emitJSON(map[string]any{
		"type": "node_joined", "timestamp_us": s.simTime,
		// node.addr: derived from the node's Ed25519 identity key at
		// node_array_add, not the sequential fallback.
		"node": nodeID, "addr": fmt.Sprintf("0x%08X", uint32(node.addr)),
		"x": x, "y": y,
	})
}

func (s *Sim) cmdRemoveNode(cmd Command) {
	node := nodeArrayFindByID(&s.nodes, cmd.NodeID)
	if node == nil {
		return
	}
	nodeDeactivate(node)

	s.emitJSON(map[string]any{
		"type": "node_left", "timestamp_us": s.simTime, "node": cmd.NodeID,
	})

	anomalyCheckPartition(&s.nodes, &s.radio, s.simTime)
}

// cmdButton forwards a face-button edge from a device card (PagerDevice.tsx)
// to the external firmware process attached under that emu-link hello id, via
// extConn.sendButton (extnode.go). Exercised by emulator/e2e's functionality
// spec, which drives buttons through the real UI rather than injecting wire
// frames directly.
func (s *Sim) cmdButton(cmd Command) {
	if s.broker == nil {
		log.Printf("btn: no broker attached (node %q)", cmd.Node)
		return
	}
	ec := s.broker.findByNode(cmd.Node)
	if ec == nil {
		log.Printf("btn: no attached node %q for id=%s edge=%s", cmd.Node, cmd.BtnID, cmd.Edge)
		return
	}
	ec.sendButton(cmd.BtnID, cmd.Edge)
}

// isHex reports whether s is exactly n hex digits. Used to reject a malformed
// network key or peer address at the broker rather than shipping it to the
// firmware, which would only drop it silently.
func isHex(s string, n int) bool {
	if len(s) != n {
		return false
	}
	for _, c := range s {
		switch {
		case c >= '0' && c <= '9', c >= 'a' && c <= 'f', c >= 'A' && c <= 'F':
		default:
			return false
		}
	}
	return true
}

// cmdProvision provisions a control-plane network key on attached firmware
// nodes at runtime (emulator/node/emu_control.c serves the "prov" message).
// An empty cmd.Node means the whole attached fleet, which is what the
// playground's one-click "provision the fleet" does; naming a node keys that
// one alone, which is how a scenario shows a still-inert neighbor next to a
// provisioned one.
//
// The key is validated here and never logged: it is the fleet secret, and the
// emulator's console stream is rendered in a browser.
func (s *Sim) cmdProvision(cmd Command) {
	if s.broker == nil {
		log.Printf("prov: no broker attached")
		return
	}
	if !isHex(cmd.Key, 64) {
		log.Printf("prov: rejected, key must be 64 hex chars")
		return
	}
	if cmd.Node != "" {
		ec := s.broker.findByNode(cmd.Node)
		if ec == nil {
			log.Printf("prov: no attached node %q", cmd.Node)
			return
		}
		ec.sendProvision(cmd.Key)
		log.Printf("prov: provisioned %s", cmd.Node)
		return
	}
	n := 0
	for _, ec := range s.broker.allConns() {
		ec.sendProvision(cmd.Key)
		n++
	}
	log.Printf("prov: provisioned %d attached node(s)", n)
}

// cmdSend makes one attached firmware node originate a message through the
// real mesh send API (emulator/node/emu_control.c serves the "send" message):
// a DM when cmd.To names a peer address, a channel broadcast otherwise.
// Unlike cmdProvision this always names a sender, because "who sent it" is the
// whole point of the message.
func (s *Sim) cmdSend(cmd Command) {
	if s.broker == nil {
		log.Printf("send: no broker attached (node %q)", cmd.Node)
		return
	}
	if cmd.Text == "" {
		log.Printf("send: rejected, empty text (node %q)", cmd.Node)
		return
	}
	if cmd.To != "" && !isHex(cmd.To, 8) {
		log.Printf("send: rejected, dest %q is not an 8-hex-digit address", cmd.To)
		return
	}
	ec := s.broker.findByNode(cmd.Node)
	if ec == nil {
		log.Printf("send: no attached node %q", cmd.Node)
		return
	}
	ec.sendMessage(cmd.Text, cmd.To)
}

// cmdAttest asks one attached firmware node to announce its identity now
// (emulator/node/emu_control.c serves the "attest" message). Always names a
// node: an announcement is about one node's identity, and blanket-announcing a
// whole fleet at once would put every node's attestation on the air in the
// same instant, which is exactly the collision the real cadence spreads out.
func (s *Sim) cmdAttest(cmd Command) {
	if s.broker == nil {
		log.Printf("attest: no broker attached (node %q)", cmd.Node)
		return
	}
	ec := s.broker.findByNode(cmd.Node)
	if ec == nil {
		log.Printf("attest: no attached node %q", cmd.Node)
		return
	}
	ec.sendAttest()
}

func (s *Sim) cmdMoveNode(cmd Command) {
	node := nodeArrayFindByID(&s.nodes, cmd.NodeID)
	if node == nil {
		return
	}
	nodeMove(node, cmd.X, cmd.Y)

	s.emitJSON(map[string]any{
		"type": "node_moved", "timestamp_us": s.simTime,
		"node": cmd.NodeID, "x": cmd.X, "y": cmd.Y,
	})
}

func (s *Sim) cmdSendMessage(cmd Command) {
	srcNode := nodeArrayFindByID(&s.nodes, cmd.Src)
	destNode := nodeArrayFindByID(&s.nodes, cmd.Dest)
	if srcNode == nil || destNode == nil {
		return
	}

	cid := C.CString(cmd.Src)
	evt := C.bridge_make_generate_msg_event(C.uint64_t(s.simTime), cid, destNode.addr)
	C.free(unsafe.Pointer(cid))
	eventQueuePush(&s.events, &evt)
}

func (s *Sim) cmdInterference(cmd Command) {
	evt := C.bridge_make_interference_start(C.uint64_t(s.simTime),
		C.float(cmd.X), C.float(cmd.Y), C.float(cmd.Radius))
	eventQueuePush(&s.events, &evt)
}

func (s *Sim) cmdSetBroadcastTelemetryMode(cmd Command) {
	mode := strings.ToLower(strings.TrimSpace(cmd.TelemetryMode))
	switch mode {
	case "", "full", "recipient_only", "off":
		if mode == "" {
			s.broadcastTelemetryMode = "full"
		} else {
			s.broadcastTelemetryMode = mode
		}
	default:
		log.Printf("invalid broadcast telemetry mode: %q", cmd.TelemetryMode)
	}
}

func (s *Sim) complete() {
	s.state = StateCompleted

	sent := uint64(s.metrics.messages_sent)
	delivered := uint64(s.metrics.delivered_packets)
	// confirmed is the TRUE confirmed-delivery count (bridge.c's
	// bridge_msg_track_confirm, fired only when a delivery receipt reaches
	// the true ORIGINATOR), as opposed to delivered above (destination reach
	// only; see bridge.c's "don't wait for receipt to arrive at source"
	// comment). confirmed <= delivered always, since a receipt can only
	// exist after the destination decoded the message.
	confirmed := uint64(s.metrics.confirmed_packets)
	dropped := uint64(s.metrics.dropped_packets)
	undelivered := uint64(0)
	if sent > delivered {
		undelivered = sent - delivered
	}

	// Per-node airtime distribution (real time-on-air transmitted), plus
	// per-tier/per-limiter denial counts: budget_denied and
	// rreq_rate_denied/rreq_fwd_denied live on each sim_node_t, summed here
	// across the fleet for final_metrics.
	var perNodeMs []uint64
	var budgetDeniedNormal, budgetDeniedCritical, budgetDeniedBroadcast, budgetDeniedReceipt uint64
	var rreqRateDenied, rreqFwdDenied uint64
	count := nodeCount(&s.nodes)
	for i := 0; i < count; i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node == nil {
			continue
		}
		perNodeMs = append(perNodeMs, uint64(node.airtime_tx_us)/1000)
		budgetDeniedNormal += uint64(node.budget_denied[C.AIRTIME_IDX_NORMAL])
		budgetDeniedCritical += uint64(node.budget_denied[C.AIRTIME_IDX_CRITICAL])
		budgetDeniedBroadcast += uint64(node.budget_denied[C.AIRTIME_IDX_BROADCAST])
		budgetDeniedReceipt += uint64(node.budget_denied[C.AIRTIME_IDX_RECEIPT])
		rreqRateDenied += uint64(node.rreq_rate_denied)
		rreqFwdDenied += uint64(node.rreq_fwd_denied)
	}
	// bridge_ext_metrics_t (bridge.h) is a single process-global struct,
	// unlike the per-node counters summed above, so it is read once rather
	// than accumulated per node.
	extMetrics := C.bridge_ext_metrics_get()
	broadcastReceiptsExpected := uint64(extMetrics.broadcast_receipts_expected)
	broadcastReceiptsRegistered := uint64(extMetrics.broadcast_receipts_registered)

	slices.Sort(perNodeMs)
	pct := func(p float64) uint64 {
		if len(perNodeMs) == 0 {
			return 0
		}
		idx := int(p * float64(len(perNodeMs)-1))
		return perNodeMs[idx]
	}
	channelUtilPct := 0.0
	offeredLoadErlangs := 0.0
	if s.duration > 0 {
		channelUtilPct = float64(s.metrics.airtime_total_us) / float64(s.duration) * 100.0
		// Erlangs: offered load as a fraction of the observation window (1.0 =
		// channel busy 100% of the time). Same ratio as channel_util_pct/100,
		// computed independently here since it is the metric's own name, not
		// a derived display value.
		offeredLoadErlangs = float64(s.metrics.airtime_total_us) / float64(s.duration)
	}

	// Per-type real time-on-air: the same accumulators
	// metrics_control_airtime_pct reads, charged once per actual TX at the
	// single sim_radio_broadcast chokepoint. ms here (not us) to match the
	// rest of this JSON's airtime fields.
	airtimeMsByType := map[string]uint64{
		"beacon":  uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_BEACON]) / 1000,
		"rreq":    uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_RREQ]) / 1000,
		"rrep":    uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_RREP]) / 1000,
		"rerr":    uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_RERR]) / 1000,
		"data":    uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_DATA]) / 1000,
		"ack":     uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_ACK]) / 1000,
		"receipt": uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_RECEIPT]) / 1000,
		"probe":   uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_PROBE]) / 1000,
		"other":   uint64(s.metrics.airtime_us_by_type[C.SIM_PKT_METRIC_OTHER]) / 1000,
	}

	// Airtime-BUDGET denials, by lane. Note these are per-TIER, not
	// per-packet-type: AIRTIME_IDX_BROADCAST covers both beacons and
	// broadcast DATA, AIRTIME_IDX_CRITICAL covers RREQ+RREP+RERR together,
	// so "attempted = sent + denied" only reconstructs at the tier level,
	// not per packet type, from these fields alone.
	budgetDeniedByTier := map[string]uint64{
		"normal":    budgetDeniedNormal,
		"critical":  budgetDeniedCritical,
		"broadcast": budgetDeniedBroadcast,
		"receipt":   budgetDeniedReceipt,
	}

	s.emitJSON(map[string]any{
		"type":             "airtime_distribution",
		"per_node_ms":      perNodeMs,
		"min_ms":           pct(0),
		"p50_ms":           pct(0.5),
		"p95_ms":           pct(0.95),
		"max_ms":           pct(1),
		"total_ms":         uint64(s.metrics.airtime_total_us) / 1000,
		"channel_util_pct": channelUtilPct,
	})

	finalMetrics := map[string]any{
		"type":          "final_metrics",
		"messages_sent": sent,
		"delivered":     delivered,
		"dropped":       dropped,
		"undelivered":   undelivered,
		// beacons_sent/rreqs_sent/rreps_sent (and every per-type count/ToA
		// bucket below) count SUCCESSFUL transmissions only, i.e. post the
		// airtime-budget gate and RREQ rate limiters: a packet that was
		// attempted but denied is NOT counted here, it is counted in
		// budget_denied_by_tier / rreq_rate_denied / rreq_fwd_denied
		// instead. "Attempted" for a given tier or limiter = sent + its
		// denied counter; see budget_denied_by_tier's own note on why that
		// reconstruction is per-tier, not per-packet-type, for the budget.
		"beacons_sent":         uint64(s.metrics.beacons_sent),
		"rreqs_sent":           uint64(s.metrics.rreqs_sent),
		"rreps_sent":           uint64(s.metrics.rreps_sent),
		"airtime_ms_by_type":   airtimeMsByType,
		"offered_load_erlangs": offeredLoadErlangs,
		"channel_util_pct":     channelUtilPct,
		// message_delivery_rate is the end-to-end scripted-message outcome:
		// delivered / all scripted messages that reached a terminal state
		// (delivered + dropped + undelivered). THE delivery number for
		// baseline and scale comparisons.
		"message_delivery_rate": messageDeliveryRate(delivered, dropped, undelivered),
		// confirmed_delivery_rate is Bramble's actual differentiator: the
		// fraction of scripted messages whose delivery receipt made it all
		// the way back to the true ORIGINATOR (confirmed), not just reached
		// the destination (delivered/message_delivery_rate above). Deliberately
		// divides by the SAME terminal-state denominator message_delivery_rate
		// uses (delivered + dropped + undelivered), not confirmed's own
		// (smaller) total, so the two rates are directly comparable side by
		// side: reach vs confirmation, matching the
		// flood_reached_rate/flood_confirmed_rate pair flood mode already
		// reports.
		"confirmed":               confirmed,
		"confirmed_delivery_rate": confirmedDeliveryRate(confirmed, delivered, dropped, undelivered),
		// control_airtime_pct is ToA-weighted: ToA(beacon+RREQ+RREP+RERR) /
		// ToA(all). control_packet_pct instead uses beacon+RREQ+RREP packet
		// COUNT / total packet count (RERR not included), kept under its own
		// name since the two measure different things and both are reported.
		"control_airtime_pct": float64(C.metrics_control_airtime_pct(&s.metrics)),
		"control_packet_pct":  float64(C.metrics_control_packet_pct(&s.metrics)),
		// Per-tier airtime-budget denials and per-limiter RREQ denials, so
		// scale runs can see how much control/data traffic the real gates
		// actually refused, not just what got sent.
		"budget_denied_by_tier": budgetDeniedByTier,
		"rreq_rate_denied":      rreqRateDenied,
		"rreq_fwd_denied":       rreqFwdDenied,
		// broadcast_receipts_expected is every (recipient, broadcast) pair
		// where a node other than the origin stored the broadcast;
		// broadcast_receipts_registered is how many of those pairs the
		// origin actually saw a delivery receipt for (bridge.c's
		// bridge_send_broadcast_delivery_receipt, gated through
		// g_ext_metrics so the count survives exactly once per pair no
		// matter how many redundant relay paths deliver the receipt).
		// receipt_return_rate is the ratio that matters for receipt
		// reliability; a broadcast-free scenario reports 1.0 (nothing was
		// owed, nothing was missed), not 0.0.
		"broadcast_receipts_expected":   broadcastReceiptsExpected,
		"broadcast_receipts_registered": broadcastReceiptsRegistered,
		"receipt_return_rate":           receiptReturnRate(broadcastReceiptsExpected, broadcastReceiptsRegistered),
	}
	s.putSharedMetrics(finalMetrics)
	s.emitJSON(finalMetrics)

	// Flood mode's own delivery bars. message_delivery_rate above is 0/0 in
	// flood runs (flood.go never touches
	// metrics.delivered_packets/dropped_packets -- see flood.go's package
	// comment) and must not be read for flood scenarios; use these fields
	// instead. flood_reached_rate is the LOOSE bar (destination ever
	// received the DATA, no confirmation, how Meshtastic is actually used);
	// flood_confirmed_rate is the STRICT bar (the true sender received a
	// flooded ACK back, the confirmation signal chosen here since it is
	// never N/A).
	if s.flood != nil {
		fl := s.flood
		reachedRate, confirmedRate := 0.0, 0.0
		if fl.totalScripted > 0 {
			reachedRate = float64(fl.reachedCount) / float64(fl.totalScripted)
			confirmedRate = float64(fl.confirmedCount) / float64(fl.totalScripted)
		}
		s.emitJSON(map[string]any{
			"type":                           "flood_final_metrics",
			"flood_hop_limit":                fl.hopLimit,
			"flood_total_scripted":           fl.totalScripted,
			"flood_reached":                  fl.reachedCount,
			"flood_reached_rate":             reachedRate,
			"flood_confirmed":                fl.confirmedCount,
			"flood_confirmed_rate":           confirmedRate,
			"flood_avg_reached_latency_ms":   avgUs(fl.reachedLatUs) / 1000.0,
			"flood_avg_confirmed_latency_ms": avgUs(fl.confirmedLatUs) / 1000.0,
			"flood_data_tx":                  fl.dataTx,
			"flood_ack_tx":                   fl.ackTx,
			"flood_relays_fired":             fl.relaysFired,
			"flood_relays_canceled":          fl.relaysCanceled,
		})
	}

	s.emitJSON(map[string]any{"type": "sim_ended"})
}

// messageDeliveryRate is delivered / (delivered + dropped + undelivered):
// the fraction of scripted messages that reached their destination, out of
// all messages that reached any terminal state. Zero-denominator (a run
// with no scripted messages) reports 0.
func messageDeliveryRate(delivered, dropped, undelivered uint64) float64 {
	total := delivered + dropped + undelivered
	if total == 0 {
		return 0.0
	}
	return float64(delivered) / float64(total)
}

// confirmedDeliveryRate is confirmed / (delivered + dropped + undelivered):
// the fraction of ALL scripted messages (the same terminal-state
// denominator messageDeliveryRate uses) whose delivery receipt made it back
// to the true originator. Deliberately NOT confirmed / (confirmed + dropped
// + undelivered): that would use a different, smaller denominator than
// message_delivery_rate and the two rates would no longer be comparable
// side by side. confirmed <= delivered always, so this rate is always <=
// message_delivery_rate. Zero-denominator (no scripted messages) reports 0.
func confirmedDeliveryRate(confirmed, delivered, dropped, undelivered uint64) float64 {
	total := delivered + dropped + undelivered
	if total == 0 {
		return 0.0
	}
	return float64(confirmed) / float64(total)
}

// receiptTxKindConfigJSON is the scenario-level A/B switch for the tx_kind_t
// an originated broadcast delivery receipt is transmitted as. A pointer so
// an omitted field (the shipped firmware kind) stays distinguishable from an
// explicit one, the same convention intermediateRREPConfigJSON and
// floodTransportConfigJSON use.
type receiptTxKindConfigJSON struct {
	ReceiptTxKind *string `json:"receipt_tx_kind"`
}

// loadReceiptTxKindConfig reads the scenario bytes' optional "receipt_tx_kind"
// field and returns the tx_kind_t to originate broadcast delivery receipts
// as. "receipt" (the default, and what firmware passes) is the kind
// tx_gate.c's lbt_defers() defers on a busy channel; "receipt_forward" is a
// real firmware kind that is NOT in that set, so choosing it reproduces
// blind-fire-on-busy behavior for measurement. Any parse failure, omitted
// field, or unrecognized value returns the default, the same fail-open
// convention as the loaders above.
func loadReceiptTxKindConfig(data []byte) int {
	var cfg receiptTxKindConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return int(C.TX_KIND_RECEIPT)
	}
	if cfg.ReceiptTxKind != nil && *cfg.ReceiptTxKind == "receipt_forward" {
		return int(C.TX_KIND_RECEIPT_FORWARD)
	}
	return int(C.TX_KIND_RECEIPT)
}

// receiptReturnRate is registered / expected: of every (recipient,
// broadcast) pair where a node other than the origin stored a broadcast
// (expected), the fraction whose delivery receipt the origin actually saw
// (registered). Unlike messageDeliveryRate/confirmedDeliveryRate, a
// zero-denominator run reports 1.0, not 0.0: a scenario with no broadcasts
// owes zero receipts, so nothing was missed, which is a passing result, not
// a failing one.
func receiptReturnRate(expected, registered uint64) float64 {
	if expected == 0 {
		return 1.0
	}
	return float64(registered) / float64(expected)
}

// restoreStdout tears down the C-stdout capture: it closes the pipe's write
// end, points fd 1 back at the saved original stdout, waits for the readPipe
// goroutine to flush the last buffered lines (which, when headless, still go
// out via origStdout), and only then releases the saved fd. Every teardown path
// funnels through here so the fd handling and close-after-flush ordering stay
// consistent.
//
// The wait is a join, not a timeout: those two closes drop the only write ends
// of the pipe (fd 1 was a dup of pipeW, and spawned node processes get their
// own StdoutPipe rather than inheriting it), so the reader is guaranteed its
// EOF and readPipe returns. Waiting a fixed duration instead would leave both a
// line still buffered at the deadline unaccounted for and origStdout closed
// under a goroutine still writing to it.
//
// pipeDone is nil only on a Sim whose reader was never started. Receiving from
// a nil channel blocks forever, so that case needs the check to skip the join
// rather than hang on it.
func (s *Sim) restoreStdout() {
	s.pipeW.Close()
	syscall.Dup2(s.origStdout, 1)
	if s.pipeDone != nil {
		<-s.pipeDone
	}
	syscall.Close(s.origStdout)
}

// emitRaw fans a ready-to-send line (already newline-terminated) out to the
// websocket broadcast callback and, when running headless, additionally to the
// saved real stdout: the pipe redirect steals the normal one, so headless runs
// write JSON to origStdout directly.
func (s *Sim) emitRaw(data []byte) {
	if s.broadcast != nil {
		s.broadcast(data)
	}
	if s.headless {
		syscall.Write(s.origStdout, data)
	}
}

// recordDeviceFB keeps the newest framebuffer event for a node so a client
// connecting later can be shown the screen the pager is already displaying.
// Every device_fb carries a whole panel, not a delta (the "partial" kind names
// the e-paper refresh mode, not a partial payload), so the newest one alone
// reconstitutes the view.
func (s *Sim) recordDeviceFB(node string, data []byte) {
	frame := make([]byte, len(data))
	copy(frame, data)
	s.fbMu.Lock()
	s.lastFB[node] = frame
	s.fbMu.Unlock()
}

// recordConsole keeps the tail of a node's console so a client connecting
// later still sees what the node has been saying. Bounded per node: this is a
// replay buffer for a fresh browser, not a log store, and the UI keeps a ring
// of its own at the same order of magnitude.
const consoleReplayLines = 200

func (s *Sim) recordConsole(node string, data []byte) {
	line := make([]byte, len(data))
	copy(line, data)
	s.fbMu.Lock()
	lines := append(s.recentConsole[node], line)
	if len(lines) > consoleReplayLines {
		lines = append([][]byte(nil), lines[len(lines)-consoleReplayLines:]...)
	}
	s.recentConsole[node] = lines
	s.fbMu.Unlock()
}

// forgetDeviceFBs drops every node's cached frame and console tail. Called
// when a scenario is torn down, so a snapshot can never show a fresh client
// the previous scenario's screens or output.
func (s *Sim) forgetDeviceFBs() {
	s.fbMu.Lock()
	s.lastFB = make(map[string][]byte)
	s.recentConsole = make(map[string][][]byte)
	s.fbMu.Unlock()
}

// SnapshotEvents returns the events a client needs to render the world that
// already exists, in the order a live client would have received them: the
// nodes that have joined, then each device's console tail and the last screen
// it painted.
//
// Without this, a UI that connects AFTER the scenario loaded sees an empty
// map forever, because joins and frames are one-shot broadcasts. That is the
// normal case for `gosim --playground`, which boots its fleet at startup and
// is then opened in a browser, and it is also what a page reload does to any
// live session.
func (s *Sim) SnapshotEvents() [][]byte {
	s.mu.RLock()
	if s.state == StateIdle {
		s.mu.RUnlock()
		return nil
	}
	var out [][]byte
	var nodeIDs []string
	for i := 0; i < nodeCount(&s.nodes); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if !bool(node.active) {
			continue
		}
		id := C.GoString(&node.id[0])
		nodeIDs = append(nodeIDs, id)
		evt := map[string]any{
			"type": "node_joined", "timestamp_us": s.simTime,
			"node": id,
			"addr": fmt.Sprintf("0x%08X", uint32(node.addr)),
			"x":    float32(node.x), "y": float32(node.y),
		}
		// Firmware nodes carry kind:"firmware", which is what makes the UI
		// give them a device card; the broker knows them by the emu-link
		// connection bound to their address.
		if _, ok := s.extConns[uint32(node.addr)]; ok {
			evt["kind"] = "firmware"
		}
		if data, err := json.Marshal(evt); err == nil {
			out = append(out, append(data, '\n'))
		}
	}
	s.mu.RUnlock()

	s.fbMu.Lock()
	for _, id := range nodeIDs {
		out = append(out, s.recentConsole[id]...)
		if frame, ok := s.lastFB[id]; ok {
			out = append(out, frame)
		}
	}
	s.fbMu.Unlock()
	return out
}

// emitJSON marshals and broadcasts a JSON event.
func (s *Sim) emitJSON(v any) {
	data, err := json.Marshal(v)
	if err != nil {
		return
	}
	data = append(data, '\n')
	s.emitRaw(data)
}

// startPipeReader launches readPipe and records the channel restoreStdout
// joins on. Every path that captures C stdout starts the reader through here so
// none of them can leave pipeDone nil and silently fall back to not waiting.
func (s *Sim) startPipeReader() {
	s.pipeDone = make(chan struct{})
	go func() {
		defer close(s.pipeDone)
		s.readPipe()
	}()
}

// readPipe reads JSON lines from the pipe (C stdout output) and broadcasts them.
func (s *Sim) readPipe() {
	scanner := bufio.NewScanner(s.pipeR)
	scanner.Buffer(make([]byte, 64*1024), 64*1024)

	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}

		// Filter out BEACON and RREQ packet events (fast byte scan)
		if shouldFilterLine(line) {
			continue
		}

		// Broadcast to clients
		out := make([]byte, len(line)+1)
		copy(out, line)
		out[len(line)] = '\n'
		s.emitRaw(out)

		if delivery, ok := websocket.BuildBroadcastDeliveryNotification(line, s.broadcastTelemetryMode); ok {
			delivery = append(delivery, '\n')
			s.emitRaw(delivery)
		}
	}
}

// shouldFilterLine returns true if the line is a BEACON or RREQ packet event.
func shouldFilterLine(line []byte) bool {
	// Quick check: must contain "packet_sent" or "packet_received"
	if !bytes.Contains(line, []byte(`"packet_sent"`)) &&
		!bytes.Contains(line, []byte(`"packet_received"`)) {
		return false
	}
	// Check for BEACON or RREQ pkt_type
	if bytes.Contains(line, []byte(`"BEACON"`)) || bytes.Contains(line, []byte(`"RREQ"`)) {
		return true
	}
	return false
}

// loadHeadless starts the C-stdout pipe reader and loads scenarioPath under the
// sim lock, the prologue both headless entry points (RunHeadless and bridge.go's
// runScenario) share. It flushes stdout and returns an error if the
// scenario does not reach StateLoaded, so callers cannot drift on the
// lock/restoreStdout-on-failure ordering. The caller creates the sim first
// (each supplies its own broadcast callback) and drives the drain afterward.
func (sim *Sim) loadHeadless(scenarioPath string) error {
	sim.startPipeReader()

	sim.mu.Lock()
	sim.cmdLoad(Command{Scenario: scenarioPath})
	sim.mu.Unlock()

	if sim.State() != StateLoaded {
		sim.restoreStdout()
		return fmt.Errorf("failed to load scenario %s", scenarioPath)
	}
	return nil
}

// RunHeadless loads a scenario and processes all events instantly, for CLI mode.
// emuListen and disableCollisions carry the --emu-listen and --no-collisions
// flags through from main so headless runs honor them.
func RunHeadless(scenarioPath, emuListen string, disableCollisions bool) error {
	sim, err := NewSim(scenarioPath, nil, true, emuListen, disableCollisions)
	if err != nil {
		return err
	}

	if err := sim.loadHeadless(scenarioPath); err != nil {
		return err
	}

	// A scenario with external firmware nodes runs on the wall clock so
	// the real node processes have time to boot, beacon, and respond. Pure
	// harness scenarios use the instant virtual-time drain below instead.
	if sim.realtime {
		return sim.runRealtimeHeadless()
	}

	// Process all events instantly
	sim.mu.Lock()
	sim.drainInstant()
	sim.mu.Unlock()

	// Flush pipe
	sim.restoreStdout()

	return nil
}

// drainInstant runs the virtual-time event loop to completion and then calls
// complete(), the shared core of both headless entry points (RunHeadless and
// bridge.go's runScenario). Events scheduled past sim.duration are not
// dispatched; any generate_message among them is counted as a drop and reported
// as a sim_ended message_dropped, so a scenario truncated by its duration
// reports the same drops however it was launched. The caller must hold sim.mu.
func (s *Sim) drainInstant() {
	s.state = StateRunning
	var evt C.sim_event_t
	for eventQueuePop(&s.events, &evt) {
		ts := getEventTimestamp(&evt)
		if s.duration > 0 && ts > s.duration {
			// Drain the event that crossed the duration plus everything after
			// it, counting each generate_message as a sim_ended drop.
			s.recordDropIfMessage(&evt)
			for eventQueuePop(&s.events, &evt) {
				s.recordDropIfMessage(&evt)
			}
			break
		}
		s.simTime = ts
		setSimTime(ts)
		s.dispatchEvent(&evt)
	}
	s.complete()
}

// recordDropIfMessage counts a past-duration generate_message event as a dropped
// packet and emits the matching sim_ended message_dropped event. Other event
// types past the duration are simply discarded.
func (s *Sim) recordDropIfMessage(evt *C.sim_event_t) {
	if evt._type == C.EVT_GENERATE_MESSAGE {
		C.metrics_record_packet_dropped(&s.metrics)
		s.emitJSON(map[string]any{
			"type": "message_dropped", "timestamp_us": s.duration,
			"reason": "sim_ended",
		})
	}
}
