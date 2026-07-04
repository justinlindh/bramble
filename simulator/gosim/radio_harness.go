package main

/*
#include <stdlib.h>
#include "bridge.h"
*/
import "C"
import (
	"fmt"
	"unsafe"
)

// radioHarness drives the C radio/collision model directly, without the
// protocol bridge. It exists so _test.go files (which cannot import "C") can
// exercise sim_radio's collision, capture, half-duplex, and time-on-air
// behavior through a pure-Go API.
type radioHarness struct {
	nodes    *C.node_array_t
	radio    *C.radio_config_t
	events   *C.event_queue_t
	rng      *C.pcg32_state_t
	metrics  *C.metrics_state_t
	anomaly  *C.node_anomaly_tracker_t // MAX_NODES-sized, indexed like nodes.nodes
	msgTrack *C.msg_tracker_t          // MAX_MSG_TRACK-sized
	beacon   *C.sim_beacon_policy_t    // firmware-default beacon policy (Task 3); mutate via setters below
}

// rxResult is one reception attempt evaluated under the collision model.
type rxResult struct {
	srcAddr  uint32
	destAddr uint32
	tsUs     uint64
	outcome  int // rxOutcome* constants
}

func newRadioHarness() *radioHarness {
	h := &radioHarness{
		nodes:    (*C.node_array_t)(C.calloc(1, C.sizeof_node_array_t)),
		radio:    (*C.radio_config_t)(C.calloc(1, C.sizeof_radio_config_t)),
		events:   (*C.event_queue_t)(C.calloc(1, C.sizeof_event_queue_t)),
		rng:      (*C.pcg32_state_t)(C.calloc(1, C.sizeof_pcg32_state_t)),
		metrics:  (*C.metrics_state_t)(C.calloc(1, C.sizeof_metrics_state_t)),
		anomaly:  (*C.node_anomaly_tracker_t)(C.calloc(C.MAX_NODES, C.sizeof_node_anomaly_tracker_t)),
		msgTrack: (*C.msg_tracker_t)(C.calloc(C.MAX_MSG_TRACK, C.sizeof_msg_tracker_t)),
		beacon:   (*C.sim_beacon_policy_t)(C.calloc(1, C.sizeof_sim_beacon_policy_t)),
	}
	C.sim_emitter_set_quiet(C.bool(true))
	C.node_array_init(h.nodes)
	C.radio_config_init(h.radio)
	C.event_queue_init(h.events)
	C.metrics_init(h.metrics)
	C.pcg32_seed(h.rng, C.uint64_t(42))
	C.sim_beacon_policy_init(h.beacon)
	return h
}

func (h *radioHarness) free() {
	C.free(unsafe.Pointer(h.nodes))
	C.free(unsafe.Pointer(h.radio))
	C.free(unsafe.Pointer(h.events))
	C.free(unsafe.Pointer(h.rng))
	C.free(unsafe.Pointer(h.metrics))
	C.free(unsafe.Pointer(h.anomaly))
	C.free(unsafe.Pointer(h.msgTrack))
	C.free(unsafe.Pointer(h.beacon))
}

func (h *radioHarness) addNode(addr uint32, x, y float32) {
	id := fmt.Sprintf("N%08X", addr)
	if nodeArrayAdd(h.nodes, id, addr, x, y) < 0 {
		panic("radioHarness: node array full")
	}
}

// activateNode runs node_activate (routing/neighbor/airtime-budget init,
// beacon RNG seeding) on an already-added node and returns it, so tests can
// drive node_tick and inspect/poke its budget-gate state directly.
func (h *radioHarness) activateNode(addr uint32) *C.sim_node_t {
	n := C.node_array_find_by_addr(h.nodes, C.uint32_t(addr))
	if n == nil {
		panic("radioHarness: unknown node")
	}
	nodeActivate(n)
	return n
}

// tick drives node_tick for one node at nowUs through the real budget gate
// (Task 1: beacon TX in node_tick), broadcasting any packets it produces
// exactly as sim.go's handleTickNode does. Returns the number of packets
// the node actually put on the air this tick (0 if the gate denied them).
func (h *radioHarness) tick(node *C.sim_node_t, nowUs uint64) int {
	var result C.node_tick_result_t
	C.node_tick(node, C.uint64_t(nowUs), h.radio, h.beacon, &result)
	for i := 0; i < int(result.count); i++ {
		C.sim_radio_broadcast(node, &result.pkts[i], h.nodes, h.radio, h.rng, h.events, h.metrics,
			C.uint64_t(nowUs))
	}
	return int(result.count)
}

// setBroadcastBudgetMs sets an artificially tiny BROADCAST-lane airtime
// budget directly on a node, bypassing the normal peer-count profile, so
// tests can force the gate to deny without waiting for realistic exhaustion.
func (h *radioHarness) setBroadcastBudgetMs(node *C.sim_node_t, ms uint32) {
	node.airtime.max_ms[C.AIRTIME_IDX_BROADCAST] = C.uint32_t(ms)
	node.airtime.tokens_ms[C.AIRTIME_IDX_BROADCAST] = C.uint32_t(ms)
}

// budgetDeniedBroadcast reads the node's BROADCAST-lane budget_denied
// counter (incremented by node_tick's beacon gate on denial).
func (h *radioHarness) budgetDeniedBroadcast(node *C.sim_node_t) uint32 {
	return uint32(node.budget_denied[C.AIRTIME_IDX_BROADCAST])
}

// applyDutyCap calls the REAL airtime_budget_set_duty_cap on a node's
// airtime budget (Task 5), exactly what bridge_apply_duty_cycle_cap does
// for a live scenario with a "radio.duty_cycle_pct" set. No sim-side duty
// math: this is a direct passthrough to the real component.
func (h *radioHarness) applyDutyCap(node *C.sim_node_t, maxDutyCyclePct uint8) {
	C.airtime_budget_set_duty_cap(&node.airtime, C.uint8_t(maxDutyCyclePct), C.bool(true))
}

// dutyCycleSet/dutyCycleCapPct read the harness's shared radio_config_t
// duty-cycle fields (Task 5 scenario schema), populated by loadScenario
// parsing a "radio.duty_cycle_pct" scenario JSON field.
func (h *radioHarness) dutyCycleSet() bool     { return bool(h.radio.duty_cycle_set) }
func (h *radioHarness) dutyCycleCapPct() uint8 { return uint8(h.radio.duty_cycle_pct) }

// applyBridgeDutyCycleCap calls the real bridge_apply_duty_cycle_cap
// (Task 5), exactly the function sim.go's cmdLoad/handleNodeJoin/cmdAddNode
// call after every node_activate. No-ops if the harness's shared radio
// config has no duty cap set, matching sim.go's own guard.
func (h *radioHarness) applyBridgeDutyCycleCap(node *C.sim_node_t) {
	if h.dutyCycleSet() {
		C.bridge_apply_duty_cycle_cap(node, h.radio.duty_cycle_pct)
	}
}

// forceBeaconDue makes the node's next beacon due at nowUs, so the very next
// tick() call attempts a beacon deterministically instead of waiting on the
// randomized first-beacon phase from node_activate.
func (h *radioHarness) forceBeaconDue(node *C.sim_node_t, nowUs uint64) {
	node.next_beacon_due_us = C.uint64_t(nowUs)
}

// nextBeaconDue reads the node's next scheduled beacon time, so tests can
// advance simulated time to exactly when the next beacon attempt is due.
func (h *radioHarness) nextBeaconDue(node *C.sim_node_t) uint64 {
	return uint64(node.next_beacon_due_us)
}

// beaconWireSize is the serialized beacon frame length (components/packet
// BEACON_SIZE), for computing the real ToA of a beacon in tests.
func beaconWireSize() int { return int(C.BEACON_SIZE) }

// setBeaconAdaptive toggles the harness's shared beacon policy between
// firmware's shipped fixed cadence (false, the default) and the opt-in
// adaptive policy driven by beacon_interval_decide (true).
func (h *radioHarness) setBeaconAdaptive(adaptive bool) {
	h.beacon.adaptive = C.bool(adaptive)
}

// setBeaconIntervalMs sets the fixed/base interval (ms) of the harness's
// shared beacon policy.
func (h *radioHarness) setBeaconIntervalMs(ms uint32) {
	h.beacon.interval_ms = C.uint32_t(ms)
}

// setNeighborCount inflates or shrinks a node's neighbor table to exactly n
// distinct entries via the real neighbor_update, so beacon_interval_decide
// (called from node_tick with the real neighbor_count()) sees a genuine
// reading rather than a stubbed one.
func (h *radioHarness) setNeighborCount(node *C.sim_node_t, n int, nowUs uint64) {
	C.neighbor_init(&node.neighbors)
	nowMs := C.uint32_t(nowUs / 1000)
	for i := 0; i < n; i++ {
		C.neighbor_update(&node.neighbors, C.uint32_t(0xE0000000+uint32(i)), C.int8_t(-60),
			C.int8_t(0), C.uint32_t(0), nowMs)
	}
}

func (h *radioHarness) setRange(r float32) {
	h.radio._range = C.float(r)
}

func (h *radioHarness) disableCollisions() {
	h.radio.collisions_enabled = C.bool(false)
}

func (h *radioHarness) disableLBT() {
	h.radio.lbt_enabled = C.bool(false)
}

func (h *radioHarness) setPHY(sf int, bwHz int, cr int) {
	h.radio.sf = C.uint8_t(sf)
	h.radio.bw_hz = C.uint32_t(bwHz)
	h.radio.cr = C.uint8_t(cr)
}

func (h *radioHarness) lbtBackoffs() uint64 {
	return uint64(h.metrics.lbt_backoffs)
}

func (h *radioHarness) frameAirtimeUs(frameBytes int) uint32 {
	return radioFrameAirtimeUs(h.radio, frameBytes)
}

func (h *radioHarness) preambleUs() uint64 {
	return radioPreambleUs(h.radio)
}

func (h *radioHarness) rssiAt(distance float32) int {
	return int(C.radio_compute_rssi(h.radio, C.float(distance)))
}

// transmit broadcasts a frame of frameBytes from srcAddr at nowUs. Unicast
// when destAddr != 0xFFFFFFFF. Calls must be made in chronological order
// (the channel log assumes time-ordered inserts, as the event loop provides).
func (h *radioHarness) transmit(srcAddr, destAddr uint32, frameBytes int, nowUs uint64) {
	src := C.node_array_find_by_addr(h.nodes, C.uint32_t(srcAddr))
	if src == nil {
		panic("radioHarness: unknown src node")
	}
	var pkt C.outbound_packet_t
	pkt.len = C.uint16_t(frameBytes)
	pkt.is_broadcast = C.bool(destAddr == 0xFFFFFFFF)
	pkt.dest_addr = C.uint32_t(destAddr)
	pkt.pkt_type = C.PKT_TYPE_DATA
	C.sim_radio_broadcast(src, &pkt, h.nodes, h.radio, h.rng, h.events, h.metrics,
		C.uint64_t(nowUs))
}

// transmitTyped is transmit with a caller-chosen wire packet type, so tests
// can drive metrics_record_tx_airtime's per-type bucketing through the real
// sim_radio_broadcast chokepoint with a known packet mix.
func (h *radioHarness) transmitTyped(srcAddr, destAddr uint32, frameBytes int, pktType byte,
	nowUs uint64) {
	src := C.node_array_find_by_addr(h.nodes, C.uint32_t(srcAddr))
	if src == nil {
		panic("radioHarness: unknown src node")
	}
	var pkt C.outbound_packet_t
	pkt.len = C.uint16_t(frameBytes)
	pkt.is_broadcast = C.bool(destAddr == 0xFFFFFFFF)
	pkt.dest_addr = C.uint32_t(destAddr)
	pkt.pkt_type = C.uint8_t(pktType)
	C.sim_radio_broadcast(src, &pkt, h.nodes, h.radio, h.rng, h.events, h.metrics,
		C.uint64_t(nowUs))
}

// receptions drains the event queue and evaluates every scheduled reception
// under the collision model, in timestamp order.
func (h *radioHarness) receptions() []rxResult {
	return h.receptionsUntil(^uint64(0))
}

// receptionsUntil evaluates scheduled receptions with timestamps <= ts.
// Long-running tests must call this before advancing simulated time with
// further transmit() calls, exactly as the real event loop interleaves
// deliveries with transmissions: channel-log pruning on insert assumes
// deliveries are not evaluated long after their air window has passed.
func (h *radioHarness) receptionsUntil(ts uint64) []rxResult {
	var out []rxResult
	var evt C.sim_event_t
	for {
		peek := eventQueuePeek(h.events)
		if peek == nil || getEventTimestamp(peek) > ts {
			break
		}
		if !eventQueuePop(h.events, &evt) {
			break
		}
		if getEventType(&evt) != C.EVT_RECEIVE_PACKET {
			continue
		}
		pkt := C.bridge_get_packet_event(&evt)
		rx := C.node_array_find_by_addr(h.nodes, pkt.dest_addr)
		if rx == nil {
			continue
		}
		out = append(out, rxResult{
			srcAddr:  uint32(pkt.src_addr),
			destAddr: uint32(pkt.dest_addr),
			tsUs:     getEventTimestamp(&evt),
			outcome:  int(C.radio_check_reception(h.radio, rx, &pkt)),
		})
	}
	return out
}

// deliverRREQ hand-builds an originator-style RREQ (prev_hop = fromAddr,
// query_id/packet_id = queryID so distinct calls are distinct RREQs to both
// rreq_dedup and the general dedup) and delivers it as an EVT_RECEIVE_PACKET
// to node `to` at nowUs, driving the real bridge_handle_receive_packet
// dispatch (dedup -> rreq_fwd_allow -> forward) exactly like a received
// radio frame, without needing the sender to actually exist as a node.
func (h *radioHarness) deliverRREQ(to *C.sim_node_t, fromAddr, rreqDestAddr, queryID uint32,
	hopLimit uint8, nowUs uint64) {
	rreq := C.rreq_build_originator(C.uint32_t(fromAddr), C.uint32_t(rreqDestAddr),
		C.uint32_t(queryID), C.uint32_t(fromAddr), C.uint8_t(hopLimit))

	var buf [C.RREQ_SIZE]C.uint8_t
	if C.bramble_rreq_serialize(&rreq, &buf[0], C.RREQ_SIZE) != C.ESP_OK {
		panic("deliverRREQ: bramble_rreq_serialize failed")
	}

	evt := C.bridge_make_receive_packet_event(C.uint64_t(nowUs), C.uint32_t(fromAddr),
		C.uint32_t(to.addr), &buf[0], C.RREQ_SIZE)
	C.bridge_handle_receive_packet(&evt, h.nodes, h.radio, h.rng, h.events, h.metrics,
		h.anomaly, h.msgTrack, C.MAX_MSG_TRACK)
}

// packetsForwarded reads a node's forwarded-packet counter (only incremented
// on an actual successful transmission, per Task 1).
func (h *radioHarness) packetsForwarded(node *C.sim_node_t) uint64 {
	return uint64(node.packets_forwarded)
}

// rreqFwdDenied reads a node's forwarded-RREQ rate-limiter denial counter.
func (h *radioHarness) rreqFwdDenied(node *C.sim_node_t) uint32 {
	return uint32(node.rreq_fwd_denied)
}

// rreqRateDenied reads a node's discovery-origination rate-limiter denial
// counter.
func (h *radioHarness) rreqRateDenied(node *C.sim_node_t) uint32 {
	return uint32(node.rreq_rate_denied)
}

// packetsSent reads a node's total-transmitted counter (sim_radio_broadcast).
func (h *radioHarness) packetsSent(node *C.sim_node_t) uint64 {
	return uint64(node.packets_sent)
}

// generateMessage drives bridge_handle_generate_message for `node` sending
// to destAddr at nowUs, exactly as the event loop does for an EVT_GENERATE_MESSAGE.
func (h *radioHarness) generateMessage(node *C.sim_node_t, destAddr uint32, nowUs uint64) {
	id := C.GoString(&node.id[0])
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	evt := C.bridge_make_generate_msg_event(C.uint64_t(nowUs), cid, C.uint32_t(destAddr))
	C.bridge_handle_generate_message(&evt, h.nodes, h.radio, h.rng, h.events, h.metrics,
		h.anomaly, h.msgTrack, C.MAX_MSG_TRACK)
}

// removePendingDiscovery drops a node's pending discovery for destAddr, so a
// later generateMessage call takes the fresh-origination (!pd) branch again,
// simulating an abandoned/exhausted discovery.
func (h *radioHarness) removePendingDiscovery(node *C.sim_node_t, destAddr uint32) {
	C.discovery_remove(&node.pending_discoveries, C.uint32_t(destAddr))
}

// Wire packet types and sim_pkt_metric_type_t indices, exposed as plain Go
// values (packet.h/sim_metrics.h constants) so _test.go files (no cgo) can
// drive transmitTyped and read airtimeUsByType without touching "C.".
var (
	pktTypeBeacon = byte(C.PKT_TYPE_BEACON)
	pktTypeRREQ   = byte(C.PKT_TYPE_RREQ)
	pktTypeRREP   = byte(C.PKT_TYPE_RREP)
	pktTypeRERR   = byte(C.PKT_TYPE_RERR)
	pktTypeData   = byte(C.PKT_TYPE_DATA)

	metricBeacon = int(C.SIM_PKT_METRIC_BEACON)
	metricRREQ   = int(C.SIM_PKT_METRIC_RREQ)
	metricRREP   = int(C.SIM_PKT_METRIC_RREP)
	metricRERR   = int(C.SIM_PKT_METRIC_RERR)
	metricData   = int(C.SIM_PKT_METRIC_DATA)
)

// airtimeUsByType reads the harness's shared metrics_state_t per-type ToA
// accumulator (Task 4), in microseconds, indexed by the metric* constants
// above.
func (h *radioHarness) airtimeUsByType(idx int) uint64 {
	return uint64(h.metrics.airtime_us_by_type[idx])
}

// controlAirtimePct reads the ToA-weighted control-plane share.
func (h *radioHarness) controlAirtimePct() float64 {
	return float64(C.metrics_control_airtime_pct(h.metrics))
}

// controlPacketPct reads the packet-COUNT-weighted control-plane share (the
// old, now-honestly-named, formula).
func (h *radioHarness) controlPacketPct() float64 {
	return float64(C.metrics_control_packet_pct(h.metrics))
}
