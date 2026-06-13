package main

/*
#cgo CFLAGS: -DBRAMBLE_SIM -std=c11 -O2 -I../../test/stubs -I../engine -I../../components/packet/include -I../../components/routing/include -I../../components/reliability/include -I../../components/dedup/include -I../../components/airtime/include -I../../components/airtime -I../../components/fragment/include -I../../components/fragment -I../../components/crypto/include -I../../components/crypto -I../../components/mailbox/include -I../../components/location/include -I../../components/group/include -I../../components/channel/include -I../../components/nvs_keys/include -I../../components/radio/include
#cgo LDFLAGS: -lm -lssl -lcrypto
#include <stdlib.h>
#include "bridge.h"
*/
import "C"
import (
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

func eventQueueCount(q *C.event_queue_t) int {
	return int(C.event_queue_count(q))
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

func radioCheckReception(config *C.radio_config_t, rx *C.sim_node_t, evt *C.sim_event_t) int {
	pkt := C.bridge_get_packet_event(evt)
	return int(C.radio_check_reception(config, rx, &pkt))
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

// Ensure imports are used
var _ = unsafe.Pointer(nil)
