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
	nodes   *C.node_array_t
	radio   *C.radio_config_t
	events  *C.event_queue_t
	rng     *C.pcg32_state_t
	metrics *C.metrics_state_t
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
		nodes:   (*C.node_array_t)(C.calloc(1, C.sizeof_node_array_t)),
		radio:   (*C.radio_config_t)(C.calloc(1, C.sizeof_radio_config_t)),
		events:  (*C.event_queue_t)(C.calloc(1, C.sizeof_event_queue_t)),
		rng:     (*C.pcg32_state_t)(C.calloc(1, C.sizeof_pcg32_state_t)),
		metrics: (*C.metrics_state_t)(C.calloc(1, C.sizeof_metrics_state_t)),
	}
	C.sim_emitter_set_quiet(C.bool(true))
	C.node_array_init(h.nodes)
	C.radio_config_init(h.radio)
	C.event_queue_init(h.events)
	C.metrics_init(h.metrics)
	C.pcg32_seed(h.rng, C.uint64_t(42))
	return h
}

func (h *radioHarness) free() {
	C.free(unsafe.Pointer(h.nodes))
	C.free(unsafe.Pointer(h.radio))
	C.free(unsafe.Pointer(h.events))
	C.free(unsafe.Pointer(h.rng))
	C.free(unsafe.Pointer(h.metrics))
}

func (h *radioHarness) addNode(addr uint32, x, y float32) {
	id := fmt.Sprintf("N%08X", addr)
	if nodeArrayAdd(h.nodes, id, addr, x, y) < 0 {
		panic("radioHarness: node array full")
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
