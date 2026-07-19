package main

/*
#cgo CFLAGS: -DBRAMBLE_SIM -std=c11 -O2 -I../../test/stubs -I../engine -I../../components/packet/include -I../../components/routing/include -I../../components/reliability/include -I../../components/dedup/include -I../../components/airtime/include -I../../components/airtime -I../../components/fragment/include -I../../components/fragment -I../../components/crypto/include -I../../components/crypto -I../../components/mailbox/include -I../../components/location/include -I../../components/channel/include -I../../components/nvs_keys/include -I../../components/radio/include -I../../components/network_key/include -I../../components/security/include -I../../components/routing_auth/include -I../../components/identity/include
#cgo LDFLAGS: -lm -lssl -lcrypto
#include <stdlib.h>
#include "bridge.h"
*/
import "C"
import (
	"fmt"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// setSimTime sets the global simulation time in microseconds.
func setSimTime(us uint64) {
	C.bridge_set_sim_time(C.uint64_t(us))
}

// loadScenario loads a scenario JSON file into the provided simulation state.
func loadScenario(path string, nodes *C.node_array_t, radio *C.radio_config_t,
	events *C.event_queue_t, rng *C.pcg32_state_t) (C.scenario_t, bool) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	// Allocate scenario_t in C heap to avoid cgo pointer-to-pointer restrictions.
	// The scenario_t contains pointers to the Sim's C state (nodes, radio, events, rng)
	// which live inside a Go-heap-allocated Sim struct. cgo disallows passing
	// a Go pointer containing other Go pointers to C, but a C-allocated struct
	// containing those same pointers is fine.
	scenario := (*C.scenario_t)(C.calloc(1, C.sizeof_scenario_t))
	defer C.free(unsafe.Pointer(scenario))

	scenario.nodes = nodes
	scenario.radio = radio
	scenario.events = events
	scenario.rng = rng

	ok := C.scenario_load_file(cpath, scenario)
	result := *scenario // copy back to stack
	return result, ok == C.bool(true)
}

// --- Node operations ---

func nodeArrayInit(arr *C.node_array_t) {
	C.node_array_init(arr)
}

func nodeArrayAdd(arr *C.node_array_t, id string, addr uint32, x, y float32) int {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	return int(C.node_array_add(arr, cid, C.uint32_t(addr), C.float(x), C.float(y)))
}

func nodeArrayFindByID(arr *C.node_array_t, id string) *C.sim_node_t {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	return C.node_array_find_by_id(arr, cid)
}

func nodeCount(arr *C.node_array_t) int {
	return int(arr.count)
}

func nodeActivate(node *C.sim_node_t) {
	C.node_activate(node)
}

func nodeDeactivate(node *C.sim_node_t) {
	C.node_deactivate(node)
}

func nodeMove(node *C.sim_node_t, x, y float32) {
	C.node_move(node, C.float(x), C.float(y))
}

// nodeMarkUnprovisioned clears bridge_node_ext_t.provisioned (mandatory-
// provisioning Task 2), making the node inert: it originates nothing
// authenticated and drops all inbound frames. join defaults nodes to
// provisioned, so the override only ever moves in this direction; the rest of
// the fleet is unaffected.
func nodeMarkUnprovisioned(idx int) {
	C.bridge_node_set_provisioned(C.int(idx), C.bool(false))
}

// nodeMarkUnendorsed clears bridge_node_ext_t.endorsed (trust-anchor campaign
// P2), making the node attest with no fleet-anchor cert, so every anchored
// receiver refuses to pin it (identity_unendorsed) while still relaying its
// MAC-valid frame. join defaults nodes to endorsed; the rest of the fleet is
// unaffected.
func nodeMarkUnendorsed(idx int) {
	C.bridge_node_set_endorsed(C.int(idx), C.bool(false))
}

// nodeMarkUnanchored clears bridge_node_ext_t.ident_pins.has_anchor (trust-
// anchor campaign P2 red-team), booting the node un-anchored (TOFU pinning)
// until a provision_anchor event hardens it. join anchors nodes to the fleet
// anchor; the rest of the fleet is unaffected.
func nodeMarkUnanchored(idx int) {
	C.bridge_node_set_anchored(C.int(idx), C.bool(false))
}

// --- Event queue ---

func eventQueueInit(q *C.event_queue_t) {
	C.event_queue_init(q)
}

func eventQueuePush(q *C.event_queue_t, e *C.sim_event_t) bool {
	return C.event_queue_push(q, e) == C.bool(true)
}

func eventQueuePop(q *C.event_queue_t, out *C.sim_event_t) bool {
	return C.event_queue_pop(q, out) == C.bool(true)
}

func eventQueuePeek(q *C.event_queue_t) *C.sim_event_t {
	return C.event_queue_peek(q)
}

// --- Event accessors ---

func getEventType(e *C.sim_event_t) C.event_type_t {
	return C.bridge_get_event_type(e)
}

func getEventTimestamp(e *C.sim_event_t) uint64 {
	return uint64(C.bridge_get_event_timestamp(e))
}

// --- Radio ---

func radioConfigInit(config *C.radio_config_t) {
	C.radio_config_init(config)
}

func radioAddInterference(config *C.radio_config_t, cx, cy, radius float32) int {
	return int(C.radio_add_interference_zone(config, C.float(cx), C.float(cy), C.float(radius)))
}

func radioClearInterference(config *C.radio_config_t, index int) {
	C.radio_clear_interference_zone(config, C.int(index))
}

// Reception outcomes under the collision model (mirror radio_rx_outcome_t).
const (
	rxOutcomeOK         = int(C.RADIO_RX_OK)
	rxOutcomeCollision  = int(C.RADIO_RX_COLLISION)
	rxOutcomeHalfDuplex = int(C.RADIO_RX_HALF_DUPLEX)
	rxOutcomeCaptured   = int(C.RADIO_RX_CAPTURED)
)

func radioFrameAirtimeUs(config *C.radio_config_t, frameBytes int) uint32 {
	return uint32(C.radio_frame_airtime_us(config, C.uint16_t(frameBytes)))
}

func radioPreambleUs(config *C.radio_config_t) uint64 {
	return uint64(C.radio_preamble_us(config))
}

// --- Metrics ---

func metricsInit(m *C.metrics_state_t) {
	C.metrics_init(m)
}

func metricsUpdateActiveNodes(m *C.metrics_state_t, count int) {
	C.metrics_update_active_nodes(m, C.int(count))
}

func metricsDeliveryRate(m *C.metrics_state_t) float64 {
	return float64(C.metrics_delivery_rate(m))
}

func metricsAvgLatencyMs(m *C.metrics_state_t) float64 {
	return float64(C.metrics_avg_latency_ms(m))
}

func metricsControlAirtimePct(m *C.metrics_state_t) float64 {
	return float64(C.metrics_control_airtime_pct(m))
}

func metricsControlPacketPct(m *C.metrics_state_t) float64 {
	return float64(C.metrics_control_packet_pct(m))
}

// --- Anomaly ---

func anomalyInit(t *C.node_anomaly_tracker_t) {
	C.anomaly_init(t)
}

func anomalyCheckPartition(nodes *C.node_array_t, radioRange float32) {
	C.anomaly_check_partition(nodes, C.float(radioRange), C.uint64_t(0), C.stdout)
}

// --- RNG ---

func pcg32Seed(rng *C.pcg32_state_t, seed uint64) {
	C.pcg32_seed(rng, C.uint64_t(seed))
}

// --- Packet handling wrappers ---

func handleReceivePacket(event *C.sim_event_t, nodes *C.node_array_t,
	radio *C.radio_config_t, rng *C.pcg32_state_t,
	events *C.event_queue_t, metrics *C.metrics_state_t,
	anomaly *C.node_anomaly_tracker_t,
	msgTrack *C.msg_tracker_t, msgTrackCount int) {
	C.bridge_handle_receive_packet(event, nodes, radio, rng, events, metrics,
		anomaly, msgTrack, C.int(msgTrackCount))
}

func handleGenerateMessage(event *C.sim_event_t, nodes *C.node_array_t,
	radio *C.radio_config_t, rng *C.pcg32_state_t,
	events *C.event_queue_t, metrics *C.metrics_state_t,
	anomaly *C.node_anomaly_tracker_t,
	msgTrack *C.msg_tracker_t, msgTrackCount int) {
	C.bridge_handle_generate_message(event, nodes, radio, rng, events, metrics,
		anomaly, msgTrack, C.int(msgTrackCount))
}

// handleFloodRelay fires a jittered channel-flood relay (Task 5) once its
// EVT_SEND_PACKET due time elapses.
func handleFloodRelay(event *C.sim_event_t, nodes *C.node_array_t,
	radio *C.radio_config_t, rng *C.pcg32_state_t,
	events *C.event_queue_t, metrics *C.metrics_state_t) {
	C.bridge_handle_flood_relay(event, nodes, radio, rng, events, metrics)
}

// handleGenerateAttestation fires a scripted identity-attestation
// origination (per-node identity Phase 3, "send_attestation" scenario
// event): the named node signs and broadcasts its (or, for the
// impersonation scenario, someone else's) address binding through the real
// firmware origination path in bridge.c.
func handleGenerateAttestation(event *C.sim_event_t, nodes *C.node_array_t,
	radio *C.radio_config_t, rng *C.pcg32_state_t,
	events *C.event_queue_t, metrics *C.metrics_state_t) {
	C.bridge_handle_generate_attestation(event, nodes, radio, rng, events, metrics)
}

// handleGenerateLocation fires a scripted GPS position broadcast (issue
// #172, "send_location" scenario event): the named node originates a
// PKT_TYPE_LOCATION broadcast through the real firmware serialization path
// in bridge.c, and every in-range receiver caches it via the real
// location_cache_update.
func handleGenerateLocation(event *C.sim_event_t, nodes *C.node_array_t,
	radio *C.radio_config_t, rng *C.pcg32_state_t,
	events *C.event_queue_t, metrics *C.metrics_state_t) {
	C.bridge_handle_generate_location(event, nodes, radio, rng, events, metrics)
}

// --- Scenario-level test harness (Phase 1 Task 1) ---
//
// _test.go files in this package avoid "C" directly (see radio_harness.go),
// so a full protocol-level scenario run (discovery + multi-hop forwarding +
// delivery receipts, as opposed to the narrow radioHarness unit tests) needs
// a Go-typed entry point. runScenarioHeadless below drives a scenario file
// exactly like RunHeadless (main.go's --headless mode) but captures the
// emitted JSON stream via an in-process callback instead of redirecting the
// process's real stdout, and keeps the completed *Sim reachable so a test
// can inspect terminal per-node state the JSON stream does not surface
// (e.g. whether a sender's pending delivery acknowledgement was ever
// cleared by a receipt).

// scenarioRunResult is the outcome of a headless scenario run captured for
// tests.
type scenarioRunResult struct {
	lines []string
	sim   *Sim
}

// runScenarioHeadless loads scenarioPath and drains its event queue to
// completion (or until the scenario's duration_ms elapses), matching
// RunHeadless's own loop.
func runScenarioHeadless(scenarioPath string) (*scenarioRunResult, error) {
	var mu sync.Mutex
	var lines []string
	sim, err := NewSim("", func(b []byte) {
		mu.Lock()
		lines = append(lines, strings.TrimRight(string(b), "\n"))
		mu.Unlock()
	}, true)
	if err != nil {
		return nil, err
	}

	// C-side fprintf JSON (message_sent, message_delivered, etc.) only
	// reaches our broadcast callback via the pipe reader goroutine, exactly
	// as RunHeadless starts it; Go-side emitJSON calls (metrics, sim_ready)
	// go straight to broadcast and do not depend on this.
	go sim.readPipe()

	sim.mu.Lock()
	sim.cmdLoad(Command{Scenario: scenarioPath})
	sim.mu.Unlock()

	if sim.State() != StateLoaded {
		return nil, fmt.Errorf("runScenarioHeadless: failed to load scenario %s", scenarioPath)
	}

	sim.mu.Lock()
	sim.state = StateRunning
	var evt C.sim_event_t
	for eventQueuePop(&sim.events, &evt) {
		ts := getEventTimestamp(&evt)
		if sim.duration > 0 && ts > sim.duration {
			// Drain remaining events past duration without dispatching them,
			// same truncation RunHeadless applies.
			for eventQueuePop(&sim.events, &evt) {
			}
			break
		}
		sim.simTime = ts
		setSimTime(ts)
		sim.dispatchEvent(&evt)
	}
	sim.complete()
	sim.mu.Unlock()

	sim.pipeW.Close()
	syscall.Dup2(sim.origStdout, 1)
	syscall.Close(sim.origStdout)
	time.Sleep(50 * time.Millisecond) // let readPipe drain

	return &scenarioRunResult{lines: lines, sim: sim}, nil
}

// Lines returns every JSON event line emitted during the run, in order.
func (r *scenarioRunResult) Lines() []string { return r.lines }

// PendingAckActive reports whether nodeID still has an active,
// unacknowledged pending-ack entry for packetID: true means that node is
// still waiting for (or exhausted retries without ever receiving) a
// delivery confirmation for that packet, i.e. it never observed the
// message as confirmed-delivered.
func (r *scenarioRunResult) PendingAckActive(nodeID string, packetID uint32) bool {
	node := nodeArrayFindByID(&r.sim.nodes, nodeID)
	if node == nil {
		return false
	}
	for i := 0; i < int(C.MAX_PENDING_ACKS); i++ {
		e := node.pending_acks.entries[i]
		if bool(e.active) && uint32(e.packet_id) == packetID {
			return true
		}
	}
	return false
}

// RouteNextHop reports whether nodeID's routing table has an entry for
// destAddr, and if so, its next_hop. Reads the C route table directly
// (like PendingAckActive reads pending_acks) rather than scraping the
// route_added JSON log line: route_added is emitted via a C-side fprintf
// through runScenarioHeadless's pipe-based stdout capture, which is only
// built to be exercised once per test process and has been observed to
// drop lines under load; a direct struct read has no such race.
func (r *scenarioRunResult) RouteNextHop(nodeID string, destAddr uint32) (uint32, bool) {
	node := nodeArrayFindByID(&r.sim.nodes, nodeID)
	if node == nil {
		return 0, false
	}
	for i := 0; i < int(node.routes.count); i++ {
		e := node.routes.entries[i]
		if uint32(e.dest_addr) == destAddr {
			return uint32(e.next_hop), true
		}
	}
	return 0, false
}
