package main

/*
#cgo CFLAGS: -DBRAMBLE_SIM -std=c11 -O2 -I../../test/stubs -I../engine -I../../components/packet/include -I../../components/routing/include -I../../components/reliability/include -I../../components/dedup/include -I../../components/airtime/include -I../../components/airtime -I../../components/fragment/include -I../../components/fragment -I../../components/crypto/include -I../../components/crypto -I../../components/mailbox/include -I../../components/location/include -I../../components/channel/include -I../../components/nvs_keys/include -I../../components/radio/include -I../../components/freq_plan/include -I../../components/network_key/include -I../../components/security/include -I../../components/routing_auth/include -I../../components/identity/include -I../../components/rollcall/include
#cgo LDFLAGS: -lm -lssl -lcrypto
#include <stdlib.h>
#include "bridge.h"
*/
import "C"
import (
	"strings"
	"sync"
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

// nodeMarkUnprovisioned clears bridge_node_ext_t.provisioned, making the node
// inert: it originates nothing authenticated and drops all inbound frames.
// join defaults nodes to provisioned, so the override only ever moves in this
// direction; the rest of the fleet is unaffected.
func nodeMarkUnprovisioned(idx int) {
	C.bridge_node_set_provisioned(C.int(idx), C.bool(false))
}

// nodeMarkUnendorsed clears bridge_node_ext_t.endorsed, making the node
// attest with no fleet-anchor cert, so every anchored receiver refuses to pin
// it (identity_unendorsed) while still relaying its MAC-valid frame. join
// defaults nodes to endorsed; the rest of the fleet is unaffected.
func nodeMarkUnendorsed(idx int) {
	C.bridge_node_set_endorsed(C.int(idx), C.bool(false))
}

// nodeMarkUnanchored clears bridge_node_ext_t.ident_pins.has_anchor, booting
// the node un-anchored (TOFU pinning) until a provision_anchor event hardens
// it. join anchors nodes to the fleet anchor; the rest of the fleet is
// unaffected.
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

// Reception outcomes under the collision model (mirror radio_rx_outcome_t).
const (
	rxOutcomeOK         = int(C.RADIO_RX_OK)
	rxOutcomeCollision  = int(C.RADIO_RX_COLLISION)
	rxOutcomeHalfDuplex = int(C.RADIO_RX_HALF_DUPLEX)
	rxOutcomeCaptured   = int(C.RADIO_RX_CAPTURED)
)

// --- Anomaly ---

func anomalyInit(t *C.node_anomaly_tracker_t) {
	C.anomaly_init(t)
}

// anomalyCheckPartition runs the reachability sweep at virtual time nowUs.
// Passing the real clock is what makes mesh_partition's emitted timestamp_us
// the detection time, the same as every other anomaly type. The whole radio
// config, not just its range, because adjacency comes from
// radio_nodes_connected: the range disk normally, the imported link graph for
// a digital-twin scenario.
func anomalyCheckPartition(nodes *C.node_array_t, radio *C.radio_config_t, nowUs uint64) {
	C.anomaly_check_partition(nodes, radio, C.uint64_t(nowUs), C.stdout)
}

// partitionComponents labels every active node with its connected-component
// index (-1 for inactive nodes), via the same anomaly_partition_components
// traversal the mesh_partition detector runs on. Returns one entry per node in
// node_array order plus the component count.
func partitionComponents(nodes *C.node_array_t, radio *C.radio_config_t) ([]int, int) {
	var comp [C.MAX_NODES]C.int
	count := int(C.anomaly_partition_components(nodes, radio, &comp[0]))
	out := make([]int, int(nodes.count))
	for i := range out {
		out[i] = int(comp[i])
	}
	return out, count
}

// --- RNG ---

func pcg32Seed(rng *C.pcg32_state_t, seed uint64) {
	C.pcg32_seed(rng, C.uint64_t(seed))
}

// --- Scenario-level test harness ---
//
// _test.go files in this package avoid "C" directly (see radio_harness.go),
// so a full protocol-level scenario run (discovery + multi-hop forwarding +
// delivery receipts, as opposed to the narrow radioHarness unit tests) needs
// a Go-typed entry point. runScenario below drives a scenario file
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

// lineCapture accumulates the newline-trimmed JSON event lines a Sim
// broadcasts. Its add method is the broadcast callback and may run on the
// C-stdout pipe-reader goroutine, so the mutex guards the append against both
// that goroutine and the test-side reader of lines.
type lineCapture struct {
	mu    sync.Mutex
	lines []string
}

func (lc *lineCapture) add(b []byte) {
	lc.mu.Lock()
	lc.lines = append(lc.lines, strings.TrimRight(string(b), "\n"))
	lc.mu.Unlock()
}

// snapshot copies the lines captured so far. Readers take a copy rather than
// the live slice because add can still be appending: the emu-link tests poll
// while the broker and the pipe reader are running, and even the headless
// callers only stop the reader at restoreStdout.
func (lc *lineCapture) snapshot() []string {
	lc.mu.Lock()
	defer lc.mu.Unlock()
	return append([]string(nil), lc.lines...)
}

// runScenario loads scenarioPath and drains its event queue to completion (or
// until the scenario's duration_ms elapses) via the same drainInstant core
// RunHeadless uses, so a duration-truncated scenario reports identical
// sim_ended drops here as it does under the real headless binary. The full
// event stream is captured into scenarioRunResult.Lines for the caller (the
// scenario tests and the digital twin's capacity probe in twin_analysis.go).
//
// NewSim's headless flag is passed false so nothing is echoed to the process's
// own stdout: the pipe capture that feeds Lines works regardless of the flag
// (it decides only whether emitRaw additionally writes to the saved real
// stdout), and every caller reads Lines rather than stdout, so echoing would
// only bury a test failure or the twin's report under the event stream.
func runScenario(scenarioPath string) (*scenarioRunResult, error) {
	var lc lineCapture
	sim, err := NewSim("", lc.add, false)
	if err != nil {
		return nil, err
	}

	// C-side fprintf JSON (message_sent, message_delivered, etc.) only
	// reaches our broadcast callback via the pipe reader goroutine that
	// loadHeadless starts, exactly as RunHeadless does; Go-side emitJSON calls
	// (metrics, sim_ready) go straight to broadcast and do not depend on this.
	if err := sim.loadHeadless(scenarioPath); err != nil {
		return nil, err
	}

	sim.mu.Lock()
	sim.drainInstant()
	sim.mu.Unlock()

	sim.restoreStdout()

	return &scenarioRunResult{lines: lc.snapshot(), sim: sim}, nil
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
// through runScenario's pipe-based stdout capture, which is only
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

// rollCallRow is one responder's line in an initiator's roll-call ledger.
type rollCallRow struct {
	Addr      uint32
	Responded bool
	Round     uint8
	AtMs      uint32 // milliseconds into the roll-call, not device uptime
}

// rollCallLedger is a Go view of one node's terminal roll-call ledger, read
// straight from the C struct the real components/rollcall code filled.
type rollCallLedger struct {
	Open       bool
	ID         uint32
	Text       string
	Anchored   bool
	Expected   int
	Responded  int
	Unattested uint32
	Overflow   uint32
	Late       uint32
	RoundsSent int
	Missing    []uint32
	Rows       []rollCallRow
}

// RollCall returns nodeID's roll-call ledger, or ok=false when that node
// never started one. Reads the C state directly rather than scraping the JSON
// stream, for the same reason RouteNextHop does.
func (r *scenarioRunResult) RollCall(nodeID string) (rollCallLedger, bool) {
	cid := C.CString(nodeID)
	defer C.free(unsafe.Pointer(cid))
	l := C.bridge_rollcall_ledger(&r.sim.nodes, cid)
	if l == nil {
		return rollCallLedger{}, false
	}

	out := rollCallLedger{
		Open:       bool(l.open),
		ID:         uint32(l.rollcall_id),
		Text:       C.GoString(&l.text[0]),
		Anchored:   bool(l.anchored),
		Expected:   int(l.expected_count),
		Responded:  int(C.rollcall_ledger_responded_count(l)),
		Unattested: uint32(l.unattested),
		Overflow:   uint32(l.overflow),
		Late:       uint32(l.late),
		RoundsSent: int(l.rounds_sent),
	}
	for i := 0; i < int(l.entry_count); i++ {
		e := l.entries[i]
		if !bool(e.used) {
			continue
		}
		row := rollCallRow{Addr: uint32(e.addr), Responded: bool(e.responded), Round: uint8(e.round)}
		if row.Responded {
			row.AtMs = uint32(e.responded_at_ms) - uint32(l.started_ms)
		}
		out.Rows = append(out.Rows, row)
	}

	var missing [C.ROLLCALL_MAX_EXPECTED]C.uint32_t
	n := int(C.rollcall_ledger_missing(l, &missing[0], C.ROLLCALL_MAX_EXPECTED))
	for i := 0; i < n && i < len(missing); i++ {
		out.Missing = append(out.Missing, uint32(missing[i]))
	}
	return out, true
}

// RollCallPendingDropped reports how many answers nodeID could not queue
// because its pending-answer queue was full.
func (r *scenarioRunResult) RollCallPendingDropped(nodeID string) uint32 {
	cid := C.CString(nodeID)
	defer C.free(unsafe.Pointer(cid))
	return uint32(C.bridge_rollcall_pending_dropped(&r.sim.nodes, cid))
}

// NodeAddr returns nodeID's simulated node address, which derives from that
// node's Ed25519 identity key and so is not knowable from the scenario file.
func (r *scenarioRunResult) NodeAddr(nodeID string) (uint32, bool) {
	node := nodeArrayFindByID(&r.sim.nodes, nodeID)
	if node == nil {
		return 0, false
	}
	return uint32(node.addr), true
}
