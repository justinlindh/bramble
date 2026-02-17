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
	"os"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
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
	Type     string  `json:"type"`
	Scenario string  `json:"scenario,omitempty"`
	Value    float64 `json:"value,omitempty"`
	NodeID   string  `json:"node_id,omitempty"`
	X        float32 `json:"x,omitempty"`
	Y        float32 `json:"y,omitempty"`
	Src      string  `json:"src,omitempty"`
	Dest     string  `json:"dest,omitempty"`
	Radius   float32 `json:"radius,omitempty"`
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

	// Sim clock
	simTime    uint64
	duration   uint64
	speed      float64
	wallStart  time.Time
	simAtStart uint64

	// Pipe for capturing C stdout output
	pipeR       *os.File
	pipeW       *os.File
	origStdout  int // saved original stdout fd

	nextAddr uint32

	cmdCh       chan Command
	stopCh      chan struct{}
	broadcast   func([]byte)
	scenarioDir  string
	lastScenario string
	headless    bool
}

// NewSim creates a new simulation engine.
func NewSim(scenarioDir string, broadcast func([]byte), headless bool) (*Sim, error) {
	s := &Sim{
		state:       StateIdle,
		speed:       1.0,
		nextAddr:    0x1000,
		cmdCh:       make(chan Command, 64),
		stopCh:      make(chan struct{}),
		broadcast:   broadcast,
		scenarioDir: scenarioDir,
		headless:    headless,
	}

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
		s.handleReceivePacket(evt)
	case C.EVT_GENERATE_MESSAGE:
		s.handleGenerateMessage(evt)
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

	var result C.node_tick_result_t
	ts := getEventTimestamp(evt)
	C.node_tick(node, C.uint64_t(ts), &result)

	// Broadcast any outbound packets
	for i := 0; i < int(result.count); i++ {
		C.sim_radio_broadcast(node, &result.pkts[i],
			&s.nodes, &s.radio, &s.rng, &s.events, &s.metrics, C.uint64_t(ts))
	}

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
	anomalyInit(&s.anomaly[idx])

	// Schedule first tick
	cid := C.CString(nodeID)
	tick := C.bridge_make_tick_event(C.uint64_t(ts+100000), cid, 0)
	C.free(unsafe.Pointer(cid))
	eventQueuePush(&s.events, &tick)

	s.emitJSON(map[string]interface{}{
		"type": "node_joined", "timestamp_us": ts,
		"node": nodeID, "addr": fmt.Sprintf("0x%08X", nd.addr),
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
		"type":          "metrics",
		"timestamp_us":  ts,
		"active_nodes":  active,
		"total_packets": uint64(s.metrics.total_packets),
		"messages_sent": uint64(s.metrics.messages_sent),
		"delivered":     uint64(s.metrics.delivered_packets),
		"dropped":       uint64(s.metrics.dropped_packets),
		"avg_latency_ms": metricsAvgLatencyMs(&s.metrics),
		"delivery_rate":  metricsDeliveryRate(&s.metrics),
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

	// Seed the RNG (scenario_load_file only seeds for stochastic mode)
	C.pcg32_seed(&s.rng, scenario.metadata.seed)
	s.duration = uint64(scenario.metadata.duration_us)
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
		anomalyInit(&s.anomaly[i])

		// Schedule initial tick (staggered by 100ms per node)
		tick := C.bridge_make_tick_event(C.uint64_t(uint64(i)*100000), &node.id[0], 0)
		eventQueuePush(&s.events, &tick)

		s.emitJSON(map[string]interface{}{
			"type": "node_joined", "timestamp_us": 0,
			"node": C.GoString(&node.id[0]),
			"addr": fmt.Sprintf("0x%08X", node.addr),
			"x": node.x, "y": node.y,
		})
	}

	// Schedule first metrics tick
	// Note: metrics ticks are pre-scheduled by scenario_load_file — no manual scheduling needed

	// Broadcast config + sim_ready
	s.emitJSON(map[string]interface{}{
		"type": "config",
		"radio_range": s.radio._range,
		"duration_us": s.duration,
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

	idx := nodeArrayAdd(&s.nodes, cmd.NodeID, uint32(addr), cmd.X, cmd.Y)
	if idx < 0 {
		log.Printf("failed to add node %s", cmd.NodeID)
		return
	}
	node := C.node_array_get(&s.nodes, C.int(idx))
	nodeActivate(node)
	anomalyInit(&s.anomaly[idx])

	// Schedule tick
	cid := C.CString(cmd.NodeID)
	tick := C.bridge_make_tick_event(C.uint64_t(s.simTime+100000), cid, 0)
	C.free(unsafe.Pointer(cid))
	eventQueuePush(&s.events, &tick)

	s.emitJSON(map[string]interface{}{
		"type": "node_joined", "timestamp_us": s.simTime,
		"node": cmd.NodeID, "addr": fmt.Sprintf("0x%08X", addr),
		"x": cmd.X, "y": cmd.Y,
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

func (s *Sim) complete() {
	s.state = StateCompleted

	s.emitJSON(map[string]interface{}{
		"type":          "final_metrics",
		"total_packets": uint64(s.metrics.total_packets),
		"messages_sent": uint64(s.metrics.messages_sent),
		"delivered":     uint64(s.metrics.delivered_packets),
		"dropped":       uint64(s.metrics.dropped_packets),
		"avg_latency_ms": metricsAvgLatencyMs(&s.metrics),
		"delivery_rate":  metricsDeliveryRate(&s.metrics),
	})
	s.emitJSON(map[string]interface{}{"type": "sim_ended"})
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
