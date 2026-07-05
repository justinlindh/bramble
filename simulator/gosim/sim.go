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
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"bramble-sim/websocket"
)

// disableCollisionModel forces the collision/half-duplex model off for every
// loaded scenario (set by the --no-collisions CLI flag).
var disableCollisionModel bool

// SimState represents the simulation state machine.
type SimState int

const (
	StateIdle SimState = iota
	StateLoaded
	StateRunning
	StatePaused
	StateCompleted
)

func (s SimState) String() string {
	switch s {
	case StateIdle:
		return "idle"
	case StateLoaded:
		return "loaded"
	case StateRunning:
		return "running"
	case StatePaused:
		return "paused"
	case StateCompleted:
		return "completed"
	default:
		return "unknown"
	}
}

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
	beacon   C.sim_beacon_policy_t // scenario-wide beacon interval policy (Task 3)

	// Sim clock
	simTime    uint64
	duration   uint64
	speed      float64
	wallStart  time.Time
	simAtStart uint64

	// Pipe for capturing C stdout output
	pipeR      *os.File
	pipeW      *os.File
	origStdout int // saved original stdout fd

	nextAddr uint32

	cmdCh                  chan Command
	stopCh                 chan struct{}
	broadcast              func([]byte)
	scenarioDir            string
	lastScenario           string
	headless               bool
	broadcastTelemetryMode string

	// Phase 2 Task 0 (flood-comparison baseline): "routing" scenario field,
	// "reactive" (default, Bramble's real firmware AODV path via
	// bridge_handle_*) or "flood" (Go-only managed-flooding mode, see
	// flood.go). flood is nil in reactive mode.
	routingMode string
	flood       *floodSim
}

// NewSim creates a new simulation engine.
func NewSim(scenarioDir string, broadcast func([]byte), headless bool) (*Sim, error) {
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
	}

	// Initialize bridge-level state
	C.bridge_init()

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
		r.Close()
		w.Close()
		return nil, fmt.Errorf("dup2: %w", err)
	}

	return s, nil
}

// Start launches the simulation goroutines.
func (s *Sim) Start() {
	go s.readPipe()
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
	// Restore stdout
	syscall.Dup2(s.origStdout, 1)
	syscall.Close(s.origStdout)
	s.pipeW.Close()
	// pipeR will get EOF and readPipe will exit
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
			s.mu.Lock()
			if s.state == StateRunning {
				s.advanceSim()
			}
			s.mu.Unlock()
		}
	}
}

func (s *Sim) handleCommand(cmd Command) {
	s.mu.Lock()
	defer s.mu.Unlock()

	switch cmd.Type {
	case "load", "start":
		s.cmdLoad(cmd)
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

	s.simTime = simNow
	setSimTime(simNow)

	// Process all events up to simNow
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

	// Check if simulation complete
	if s.duration > 0 && simNow >= s.duration {
		s.complete()
	}
}

func (s *Sim) dispatchEvent(evt *C.sim_event_t) {
	evtType := getEventType(evt)

	switch evtType {
	case C.EVT_TICK_NODE:
		s.handleTickNode(evt)
	case C.EVT_RECEIVE_PACKET:
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
			s.handleFloodRelay(evt)
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
		// Per-node identity Phase 3: attestations always go through the
		// real firmware C path in bridge.c (there is no Go-model flood
		// equivalent; scenarios that script send_attestation use the
		// default routing mode).
		s.handleGenerateAttestation(evt)
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

	// Phase 2 Task 0: managed flooding has no periodic control-plane duty
	// (no beacons -- there is no neighbor table for one to serve -- no
	// route maintenance, no per-hop retransmit ladder). Nothing to do on a
	// tick, and nothing depends on rescheduling it: flood mode's own
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

	// Check for retransmissions (Phase 1)
	C.bridge_handle_retransmit(node, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics, C.uint64_t(ts))

	// Reschedule next tick (1 second later)
	nextTick := C.bridge_make_tick_event(C.uint64_t(ts+1000000), &tickData.node_id[0], tickData.tick_seq+1)
	eventQueuePush(&s.events, &nextTick)
}

func (s *Sim) handleReceivePacket(evt *C.sim_event_t) {
	handleReceivePacket(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.anomaly[0], &s.msgTrack[0], C.MAX_MSG_TRACK)
}

func (s *Sim) handleGenerateMessage(evt *C.sim_event_t) {
	handleGenerateMessage(evt, &s.nodes, &s.radio, &s.rng, &s.events,
		&s.metrics, &s.anomaly[0], &s.msgTrack[0], C.MAX_MSG_TRACK)
}

func (s *Sim) handleGenerateAttestation(evt *C.sim_event_t) {
	handleGenerateAttestation(evt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics)
}

// handleFloodRelay fires a jittered channel-flood relay (Task 5): see
// bridge.c's _handle_data broadcast branch, which schedules these via
// EVT_SEND_PACKET (repurposed; previously declared but unused).
func (s *Sim) handleFloodRelay(evt *C.sim_event_t) {
	handleFloodRelay(evt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics)
}

// applyDutyCycleCap re-applies the scenario's optional regulatory
// duty-cycle cap (Task 5) to a node's real airtime budget via the real
// airtime_budget_set_duty_cap. Must run after every node_activate, since
// node_activate's airtime_budget_init resets the cap; no-op if the
// scenario's "radio" block has no duty_cycle_pct (unlimited, today's
// behavior).
func (s *Sim) applyDutyCycleCap(node *C.sim_node_t) {
	if bool(s.radio.duty_cycle_set) {
		C.bridge_apply_duty_cycle_cap(node, s.radio.duty_cycle_pct)
	}
}

func (s *Sim) handleNodeJoin(evt *C.sim_event_t) {
	nd := C.bridge_get_node_event(evt)
	nodeID := C.GoString(&nd.node_id[0])
	ts := getEventTimestamp(evt)

	idx := nodeArrayAdd(&s.nodes, nodeID, uint32(nd.addr), float32(nd.x), float32(nd.y))
	if idx < 0 {
		log.Printf("failed to add node %s", nodeID)
		return
	}
	node := C.node_array_get(&s.nodes, C.int(idx))
	nodeActivate(node)
	s.applyDutyCycleCap(node)
	anomalyInit(&s.anomaly[idx])

	// Phase 6: Initialize extended node state (mailbox, location, etc.)
	// node.addr, not nd.addr: node_array_add derives the address from the
	// node's Ed25519 identity key (Phase 4 rebind).
	C.bridge_handle_node_join_ext(C.int(idx), node.addr,
		nd.x, nd.y, C.uint64_t(ts))

	// Schedule first tick
	cid := C.CString(nodeID)
	tick := C.bridge_make_tick_event(C.uint64_t(ts+100000), cid, 0)
	C.free(unsafe.Pointer(cid))
	eventQueuePush(&s.events, &tick)

	s.emitJSON(map[string]interface{}{
		"type": "node_joined", "timestamp_us": ts,
		"node": nodeID, "addr": fmt.Sprintf("0x%08X", uint32(node.addr)),
		"x": nd.x, "y": nd.y,
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

	s.emitJSON(map[string]interface{}{
		"type": "node_left", "timestamp_us": ts, "node": nodeID,
	})

	anomalyCheckPartition(&s.nodes, float32(s.radio._range))
}

func (s *Sim) handleNodeMove(evt *C.sim_event_t) {
	nd := C.bridge_get_node_event(evt)
	nodeID := C.GoString(&nd.node_id[0])
	ts := getEventTimestamp(evt)

	node := nodeArrayFindByID(&s.nodes, nodeID)
	if node != nil {
		nodeMove(node, float32(nd.x), float32(nd.y))
	}

	s.emitJSON(map[string]interface{}{
		"type": "node_moved", "timestamp_us": ts,
		"node": nodeID, "x": nd.x, "y": nd.y,
	})
}

func (s *Sim) handleInterferenceStart(evt *C.sim_event_t) {
	idata := C.bridge_get_interference_event(evt)
	radioAddInterference(&s.radio, float32(idata.center_x), float32(idata.center_y), float32(idata.radius))
}

func (s *Sim) handleInterferenceEnd(evt *C.sim_event_t) {
	idata := C.bridge_get_interference_event(evt)
	radioClearInterference(&s.radio, int(idata.zone_index))
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
	metricsUpdateActiveNodes(&s.metrics, active)

	s.emitJSON(map[string]interface{}{
		"type":                  "metrics",
		"timestamp_us":          ts,
		"active_nodes":          active,
		"total_packets":         uint64(s.metrics.total_packets),
		"messages_sent":         uint64(s.metrics.messages_sent),
		"delivered":             uint64(s.metrics.delivered_packets),
		"dropped":               uint64(s.metrics.dropped_packets),
		"retried":               uint64(s.metrics.messages_retried),
		"delivered_on_retry":    uint64(s.metrics.messages_delivered_retry),
		"dedup_dropped":         uint64(s.metrics.dedup_dropped),
		"airtime_deferred":      uint64(s.metrics.airtime_deferred),
		"fragments_sent":        uint64(s.metrics.fragments_sent),
		"fragments_reassembled": uint64(s.metrics.fragments_reassembled),
		"reassembly_timeout":    uint64(s.metrics.reassembly_timeout),
		"crypto_encrypted":      uint64(s.metrics.crypto_encrypted),
		"crypto_decrypted":      uint64(s.metrics.crypto_decrypted),
		"crypto_auth_failed":    uint64(s.metrics.crypto_auth_failed),
		"collisions":            uint64(s.metrics.collisions),
		"half_duplex_drops":     uint64(s.metrics.half_duplex_drops),
		"capture_wins":          uint64(s.metrics.capture_wins),
		"lbt_backoffs":          uint64(s.metrics.lbt_backoffs),
		"receptions_ok":         uint64(s.metrics.receptions_ok),
		"channel_log_overflow":  uint64(s.radio.channel.overflow_drops),
		"airtime_total_ms":      uint64(s.metrics.airtime_total_us) / 1000,
		"avg_latency_ms":        metricsAvgLatencyMs(&s.metrics),
		"delivery_rate":         metricsDeliveryRate(&s.metrics),
	})

	// Check black holes
	for i := 0; i < count; i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node != nil && bool(node.active) {
			C.anomaly_check_blackhole(&s.anomaly[i].blackhole, C.uint64_t(ts), C.stdout, &node.id[0])
		}
	}
	// Note: metrics ticks are pre-scheduled by scenario_load_file — no rescheduling needed
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

	// Reset C state
	nodeArrayInit(&s.nodes)
	radioConfigInit(&s.radio)
	eventQueueInit(&s.events)
	metricsInit(&s.metrics)
	C.bridge_msg_track_init(&s.msgTrack[0], C.MAX_MSG_TRACK)
	for i := 0; i < C.MAX_NODES; i++ {
		anomalyInit(&s.anomaly[i])
	}

	scenario, ok := loadScenario(scenarioPath, &s.nodes, &s.radio, &s.events, &s.rng)
	if !ok {
		log.Printf("failed to load scenario: %s", scenarioPath)
		return
	}

	// Phase 2 Task 0: optional "routing"/"flood_hop_limit" fields, read
	// directly off the scenario file (see flood.go's loadRoutingConfig),
	// independent of the C-side cJSON parse above. Defaults to "reactive"
	// (today's only behavior) for every scenario that omits "routing".
	routingMode, floodHopLimit := loadRoutingConfig(scenarioPath)
	s.routingMode = routingMode
	if routingMode == "flood" {
		s.flood = newFloodSim(floodHopLimit)
	} else {
		s.flood = nil
	}

	// Phase 2 "save reactive routing" Part B: optional "intermediate_rrep"
	// scenario field, read the same way as "routing" above (independent Go-
	// side JSON read, so this schema extension needs no C-side sim_scenario
	// changes). Defaults to true (firmware's always-on shipped behavior);
	// explicitly re-applied on every run (not just when disabling) so one
	// scenario's setting never leaks into the next run in the same process
	// (see bridge.h's doc comment on bridge_set_intermediate_rrep_enabled).
	C.bridge_set_intermediate_rrep_enabled(C.bool(loadIntermediateRREPConfig(scenarioPath)))

	// Flooding F1 Task 1: optional "flood_transport" scenario field, read the
	// same way as "intermediate_rrep" above (independent Go-side JSON read,
	// no C-side sim_scenario changes needed). Drives the REAL firmware flood
	// transport through bridge.c (see flood.go's loadFloodTransportConfig doc
	// comment); distinct from s.routingMode's Go-only "flood" MODEL.
	// Defaults to false (firmware's shipped NVS default); re-applied on every
	// load so one scenario's setting never leaks into the next run in the
	// same process.
	floodTransport, floodTransportHopLimit := loadFloodTransportConfig(scenarioPath)
	C.bridge_set_flood_transport_enabled(C.bool(floodTransport))
	// Flooding F1 finalize: optional "flood_hop_limit" scenario field drives the
	// flood-transport origination hop budget (firmware's s_flood_hop_limit),
	// re-applied on every load. bridge_set_flood_hop_limit clamps to the
	// firmware range; a farther-reaching flood needs a larger value here.
	C.bridge_set_flood_hop_limit(C.uint8_t(floodTransportHopLimit))

	// Seed the RNG (scenario_load_file only seeds for stochastic mode)
	C.pcg32_seed(&s.rng, scenario.metadata.seed)
	if disableCollisionModel {
		s.radio.collisions_enabled = C.bool(false)
	}
	s.duration = uint64(scenario.metadata.duration_us)
	s.beacon = scenario.beacon
	s.simTime = 0
	s.speed = 1.0
	s.nextAddr = 0x1000
	s.state = StateLoaded

	// Broadcast sim_reset
	s.emitJSON(map[string]interface{}{"type": "sim_reset"})

	// Broadcast node_joined for each initial node
	count := nodeCount(&s.nodes)
	for i := 0; i < count; i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if node == nil {
			continue
		}
		nodeActivate(node)
		s.applyDutyCycleCap(node)
		anomalyInit(&s.anomaly[i])

		// Initialize extended node state exactly like a dynamic join does
		// (handleNodeJoin): position, and (per-node identity Phase 3) the
		// node's Ed25519 identity keypair. Initial scenario nodes used to
		// skip this, which left them without identities and unable to
		// originate or pin attestations.
		C.bridge_handle_node_join_ext(C.int(i), C.uint32_t(node.addr),
			node.x, node.y, C.uint64_t(0))

		// Schedule initial tick (staggered by 100ms per node)
		tick := C.bridge_make_tick_event(C.uint64_t(uint64(i)*100000), &node.id[0], 0)
		eventQueuePush(&s.events, &tick)

		s.emitJSON(map[string]interface{}{
			"type": "node_joined", "timestamp_us": 0,
			"node": C.GoString(&node.id[0]),
			"addr": fmt.Sprintf("0x%08X", node.addr),
			"x":    node.x, "y": node.y,
		})
	}

	// Schedule first metrics tick
	// Note: metrics ticks are pre-scheduled by scenario_load_file — no manual scheduling needed

	// Broadcast config + sim_ready
	s.emitJSON(map[string]interface{}{
		"type":        "config",
		"radio_range": s.radio._range,
		"duration_us": s.duration,
		"collisions":  bool(s.radio.collisions_enabled),
		"sf":          uint8(s.radio.sf),
		"bw_hz":       uint32(s.radio.bw_hz),
		"cr":          uint8(s.radio.cr),
	})
	s.emitJSON(map[string]interface{}{"type": "sim_ready"})

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

	s.emitJSON(map[string]interface{}{
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

	s.emitJSON(map[string]interface{}{
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
	s.emitJSON(map[string]interface{}{
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

	s.emitJSON(map[string]interface{}{
		"type": "node_joined", "timestamp_us": s.simTime,
		// node.addr: derived from the node's Ed25519 identity key at
		// node_array_add (Phase 4 rebind), not the sequential fallback.
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

	s.emitJSON(map[string]interface{}{
		"type": "node_left", "timestamp_us": s.simTime, "node": cmd.NodeID,
	})

	anomalyCheckPartition(&s.nodes, float32(s.radio._range))
}

func (s *Sim) cmdMoveNode(cmd Command) {
	node := nodeArrayFindByID(&s.nodes, cmd.NodeID)
	if node == nil {
		return
	}
	nodeMove(node, cmd.X, cmd.Y)

	s.emitJSON(map[string]interface{}{
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
	cid := C.CString("")
	_ = cid
	C.free(unsafe.Pointer(cid))

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
	// Phase 2 "save reactive routing" Part A: confirmed is the TRUE
	// confirmed-delivery count (bridge.c's bridge_msg_track_confirm, fired
	// only when a delivery receipt reaches the true ORIGINATOR), as opposed
	// to delivered above (destination reach only; see bridge.c's "don't
	// wait for receipt to arrive at source" comment). confirmed <= delivered
	// always, since a receipt can only exist after the destination decoded
	// the message.
	confirmed := uint64(s.metrics.confirmed_packets)
	dropped := uint64(s.metrics.dropped_packets)
	undelivered := uint64(0)
	if sent > delivered {
		undelivered = sent - delivered
	}

	// Per-node airtime distribution (real time-on-air transmitted), plus
	// per-tier/per-limiter denial counts: budget_denied (Task 1) and
	// rreq_rate_denied/rreq_fwd_denied (Task 2) live on each sim_node_t,
	// summed here across the fleet for final_metrics.
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
	sort.Slice(perNodeMs, func(i, j int) bool { return perNodeMs[i] < perNodeMs[j] })
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

	// Per-type real time-on-air (Task 4): same accumulators
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

	// Airtime-BUDGET denials (Task 1), by lane. Note these are per-TIER, not
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

	s.emitJSON(map[string]interface{}{
		"type":             "airtime_distribution",
		"per_node_ms":      perNodeMs,
		"min_ms":           pct(0),
		"p50_ms":           pct(0.5),
		"p95_ms":           pct(0.95),
		"max_ms":           pct(1),
		"total_ms":         uint64(s.metrics.airtime_total_us) / 1000,
		"channel_util_pct": channelUtilPct,
	})

	s.emitJSON(map[string]interface{}{
		"type":                  "final_metrics",
		"total_packets":         uint64(s.metrics.total_packets),
		"messages_sent":         sent,
		"delivered":             delivered,
		"dropped":               dropped,
		"undelivered":           undelivered,
		"retried":               uint64(s.metrics.messages_retried),
		"delivered_on_retry":    uint64(s.metrics.messages_delivered_retry),
		"dedup_dropped":         uint64(s.metrics.dedup_dropped),
		"airtime_deferred":      uint64(s.metrics.airtime_deferred),
		"fragments_sent":        uint64(s.metrics.fragments_sent),
		"fragments_reassembled": uint64(s.metrics.fragments_reassembled),
		"reassembly_timeout":    uint64(s.metrics.reassembly_timeout),
		"crypto_encrypted":      uint64(s.metrics.crypto_encrypted),
		"crypto_decrypted":      uint64(s.metrics.crypto_decrypted),
		"crypto_auth_failed":    uint64(s.metrics.crypto_auth_failed),
		// beacons_sent/rreqs_sent/rreps_sent (and every per-type count/ToA
		// bucket below) count SUCCESSFUL transmissions only, i.e. post the
		// Task 1 airtime-budget gate and Task 2 RREQ rate limiters: a
		// packet that was attempted but denied is NOT counted here, it is
		// counted in budget_denied_by_tier / rreq_rate_denied / rreq_fwd_denied
		// instead. "Attempted" for a given tier or limiter = sent + its
		// denied counter; see budget_denied_by_tier's own note on why that
		// reconstruction is per-tier, not per-packet-type, for the budget.
		"beacons_sent":         uint64(s.metrics.beacons_sent),
		"rreqs_sent":           uint64(s.metrics.rreqs_sent),
		"rreps_sent":           uint64(s.metrics.rreps_sent),
		"collisions":           uint64(s.metrics.collisions),
		"half_duplex_drops":    uint64(s.metrics.half_duplex_drops),
		"capture_wins":         uint64(s.metrics.capture_wins),
		"lbt_backoffs":         uint64(s.metrics.lbt_backoffs),
		"receptions_ok":        uint64(s.metrics.receptions_ok),
		"channel_log_overflow": uint64(s.radio.channel.overflow_drops),
		"airtime_total_ms":     uint64(s.metrics.airtime_total_us) / 1000,
		"airtime_ms_by_type":   airtimeMsByType,
		"offered_load_erlangs": offeredLoadErlangs,
		"channel_util_pct":     channelUtilPct,
		"avg_latency_ms":       metricsAvgLatencyMs(&s.metrics),
		// delivery_rate divides delivered by total_packets (every frame of
		// every type on the air, beacons included), which is NOT a message
		// delivery figure and understates end-to-end delivery by an order of
		// magnitude in control-heavy runs. Kept under its old name for
		// continuity; use message_delivery_rate for the honest number.
		"delivery_rate": metricsDeliveryRate(&s.metrics),
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
		// control_airtime_pct is now genuinely ToA-weighted:
		// ToA(beacon+RREQ+RREP+RERR) / ToA(all). control_packet_pct is the
		// OLD formula (beacon+RREQ+RREP packet COUNT / total packet count,
		// RERR not included) kept under its own honest name for continuity.
		"control_airtime_pct": metricsControlAirtimePct(&s.metrics),
		"control_packet_pct":  metricsControlPacketPct(&s.metrics),
		// Per-tier airtime-budget denials (Task 1) and per-limiter RREQ
		// denials (Task 2), so scale runs can see how much control/data
		// traffic the real gates actually refused, not just what got sent.
		"budget_denied_by_tier": budgetDeniedByTier,
		"rreq_rate_denied":      rreqRateDenied,
		"rreq_fwd_denied":       rreqFwdDenied,
	})

	// Phase 2 Task 0 (flood-comparison baseline): flood mode's own delivery
	// bars. message_delivery_rate above is 0/0 in flood runs (flood.go never
	// touches metrics.delivered_packets/dropped_packets -- see flood.go's
	// package comment) and must not be read for flood scenarios; use these
	// fields instead. flood_reached_rate is the LOOSE bar (destination ever
	// received the DATA, no confirmation, how Meshtastic is actually used);
	// flood_confirmed_rate is the STRICT bar (the true sender received a
	// flooded ACK back, this task's chosen non-N/A confirmation signal).
	if s.flood != nil {
		fl := s.flood
		reachedRate, confirmedRate := 0.0, 0.0
		if fl.totalScripted > 0 {
			reachedRate = float64(fl.reachedCount) / float64(fl.totalScripted)
			confirmedRate = float64(fl.confirmedCount) / float64(fl.totalScripted)
		}
		s.emitJSON(map[string]interface{}{
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

	s.emitJSON(map[string]interface{}{"type": "sim_ended"})
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

// emitJSON marshals and broadcasts a JSON event.
func (s *Sim) emitJSON(v interface{}) {
	data, err := json.Marshal(v)
	if err != nil {
		return
	}
	data = append(data, '\n')
	if s.broadcast != nil {
		s.broadcast(data)
	}
	if s.headless {
		// Write to original stdout (not the pipe-redirected one)
		syscall.Write(s.origStdout, data)
	}
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
		if s.broadcast != nil {
			s.broadcast(out)
		}
		if s.headless {
			syscall.Write(s.origStdout, out)
		}

		if delivery, ok := websocket.BuildBroadcastDeliveryNotification(line, s.broadcastTelemetryMode); ok {
			delivery = append(delivery, '\n')
			if s.broadcast != nil {
				s.broadcast(delivery)
			}
			if s.headless {
				syscall.Write(s.origStdout, delivery)
			}
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

// RunHeadless loads a scenario and processes all events instantly, for CLI mode.
func RunHeadless(scenarioPath string) error {
	sim, err := NewSim(scenarioPath, nil, true)
	if err != nil {
		return err
	}

	// Start pipe reader
	go sim.readPipe()

	// Load scenario
	sim.mu.Lock()
	sim.cmdLoad(Command{Scenario: scenarioPath})
	sim.mu.Unlock()

	if sim.State() != StateLoaded {
		return fmt.Errorf("failed to load scenario")
	}

	// Process all events instantly
	sim.mu.Lock()
	sim.state = StateRunning
	var evt C.sim_event_t
	for eventQueuePop(&sim.events, &evt) {
		ts := getEventTimestamp(&evt)
		if sim.duration > 0 && ts > sim.duration {
			// Count remaining generate_message events as dropped
			if evt._type == C.EVT_GENERATE_MESSAGE {
				C.metrics_record_packet_dropped(&sim.metrics)
				sim.emitJSON(map[string]interface{}{
					"type": "message_dropped", "timestamp_us": sim.duration,
					"reason": "sim_ended",
				})
			}
			// Drain remaining events past duration
			for eventQueuePop(&sim.events, &evt) {
				if evt._type == C.EVT_GENERATE_MESSAGE {
					C.metrics_record_packet_dropped(&sim.metrics)
					sim.emitJSON(map[string]interface{}{
						"type": "message_dropped", "timestamp_us": sim.duration,
						"reason": "sim_ended",
					})
				}
			}
			break
		}
		sim.simTime = ts
		setSimTime(ts)
		sim.dispatchEvent(&evt)
	}
	sim.complete()
	sim.mu.Unlock()

	// Flush pipe
	sim.pipeW.Close()
	syscall.Dup2(sim.origStdout, 1)
	time.Sleep(100 * time.Millisecond) // let readPipe drain

	return nil
}
