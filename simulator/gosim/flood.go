package main

/*
#include <stdlib.h>
#include "bridge.h"
*/
import "C"
import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"strings"
	"unsafe"
)

// flood.go implements a flood-comparison baseline: a Meshtastic-style
// MANAGED-FLOODING routing mode, selected per-scenario via the top-level
// "routing":"flood" JSON field (default "reactive", Bramble's real
// firmware AODV path via bridge.c/bridge_handle_*).
//
// This is a SIM-LAYER MODEL of Meshtastic's flood strategy, not Meshtastic
// firmware: no route discovery, no reverse routes, fire-and-forget
// broadcast, dedup by (packet_id, src_addr), decrementing hop_limit
// (default 3, Meshtastic's shipped default), an SNR-derived rebroadcast
// delay (weaker SNR -> shorter delay, so the node with the least redundant
// coverage tends to relay first, mirroring Meshtastic's contention
// heuristic), and duplicate-triggered cancellation of an own pending
// rebroadcast once another relay is overheard. It goes through the exact
// same physics as the reactive path -- C.sim_radio_broadcast for airtime/
// collision/capture, C.airtime_budget_can_transmit/debit on the BROADCAST
// tier for admission -- so the two transports are compared on identical
// radio conditions, only the routing strategy differs.
//
// Known approximations against real Meshtastic firmware:
//   - No periodic beacons/NodeInfo/position broadcasts: managed flooding
//     has no neighbor table to maintain, so there is nothing for a
//     Bramble-style beacon to serve; EVT_TICK_NODE is a no-op in flood mode
//     (see sim.go's handleTickNode).
//   - The rebroadcast-delay formula (floodRebroadcastDelayMs) approximates
//     Meshtastic's actual contention-window/SNR heuristic, not a
//     byte-for-byte port of its firmware.
//   - The flooded ACK (floodMsgAck) models Meshtastic's real want_ack
//     flooded-acknowledgement behavior (also hop-limited, also flooded, no
//     reverse route) so the STRICT delivery-with-confirmation bar has a
//     genuine, non-N/A signal instead of being N/A.

const (
	floodMsgData byte = 0
	floodMsgAck  byte = 1

	// floodFrameSize: type(1) + hop_limit(1) + packet_id(4) + src_addr(4) +
	// dest_addr(4) + corr_id(4) = 18 bytes. Deliberately in the same
	// ballpark as reactive's HEADER_SIZE (12 bytes, packet.h) for the
	// legacy scenarios' header-only (payload_size=0) DATA frames, so
	// per-packet ToA is a like-for-like comparison.
	floodFrameSize = 18

	floodDefaultHopLimit = 3 // Meshtastic's shipped default hop_limit.

	// Rebroadcast-delay heuristic (documented approximation of Meshtastic's
	// SNR-based contention window): weaker SNR -> shorter delay, so the
	// hearer with the least redundant coverage tends to key up first.
	floodRebroadcastBaseMs   = 100
	floodRebroadcastSpreadMs = 400
	floodRebroadcastMaxSNRdB = 20

	// floodSuppressAfterHeard: a node cancels its own pending rebroadcast
	// once it has overheard this many OTHER relays of the same
	// (packet_id, src_addr) while waiting out its delay. Meshtastic cancels
	// on the first overheard relay; kept as a named constant so a sweep can
	// try a higher tolerance without touching the algorithm.
	floodSuppressAfterHeard = 2
)

// floodKey identifies one flood transmission for dedup/suppression
// purposes, scoped per receiving node (see floodSim.seen/pending, both
// keyed first by the local node's address).
type floodKey struct {
	msgType  byte
	packetID uint32
	srcAddr  uint32
}

// floodFrame is the decoded form of a flood.go wire frame (see
// encodeFloodFrame). srcAddr/destAddr are payload-level (the true
// originator and final intended recipient), independent of the radio
// layer's own per-hop src/dest (event.data.packet.{src,dest}_addr), exactly
// as a real flood packet's embedded header differs from a raw radio frame's
// immediate hop.
type floodFrame struct {
	msgType  byte
	hopLimit byte
	packetID uint32
	srcAddr  uint32
	destAddr uint32
	corrID   uint32 // ACK only: the original DATA packet's packetID.
}

func encodeFloodFrame(msgType, hopLimit byte, packetID, srcAddr, destAddr, corrID uint32) []byte {
	buf := make([]byte, floodFrameSize)
	buf[0] = msgType
	buf[1] = hopLimit
	binary.LittleEndian.PutUint32(buf[2:6], packetID)
	binary.LittleEndian.PutUint32(buf[6:10], srcAddr)
	binary.LittleEndian.PutUint32(buf[10:14], destAddr)
	binary.LittleEndian.PutUint32(buf[14:18], corrID)
	return buf
}

func decodeFloodFrame(buf []byte) (floodFrame, bool) {
	if len(buf) < floodFrameSize {
		return floodFrame{}, false
	}
	return floodFrame{
		msgType:  buf[0],
		hopLimit: buf[1],
		packetID: binary.LittleEndian.Uint32(buf[2:6]),
		srcAddr:  binary.LittleEndian.Uint32(buf[6:10]),
		destAddr: binary.LittleEndian.Uint32(buf[10:14]),
		corrID:   binary.LittleEndian.Uint32(buf[14:18]),
	}, true
}

// floodPending is one node's scheduled-but-not-yet-fired rebroadcast.
type floodPending struct {
	heard    int
	canceled bool
	frame    []byte
	pktType  C.uint8_t
}

// floodOrigin tracks one scripted message's lifecycle for the two delivery
// bars: reached (LOOSE, destination ever received the DATA) and confirmed
// (STRICT, the true sender received a flooded ACK back). Keyed by the
// DATA packet's packetID in floodSim.origins.
type floodOrigin struct {
	srcAddr, destAddr uint32
	sentUs            uint64
	reached           bool
	reachedUs         uint64
	confirmed         bool
	confirmedUs       uint64
}

// floodSim is the whole of the managed-flooding routing mode's state,
// separate from and parallel to the reactive path's sim_node_t fields
// (routing_table_t etc, all unused in flood mode). One instance per loaded
// scenario, reset in sim.go's cmdLoad.
type floodSim struct {
	hopLimit byte

	seen    map[uint32]map[floodKey]bool
	pending map[uint32]map[floodKey]*floodPending
	origins map[uint32]*floodOrigin

	totalScripted  int
	reachedCount   int
	confirmedCount int
	reachedLatUs   []uint64
	confirmedLatUs []uint64
	dataTx         int
	ackTx          int
	relaysFired    int
	relaysCanceled int
}

func newFloodSim(hopLimit byte) *floodSim {
	return &floodSim{
		hopLimit: hopLimit,
		seen:     make(map[uint32]map[floodKey]bool),
		pending:  make(map[uint32]map[floodKey]*floodPending),
		origins:  make(map[uint32]*floodOrigin),
	}
}

func (f *floodSim) markSeen(nodeAddr uint32, key floodKey) {
	m := f.seen[nodeAddr]
	if m == nil {
		m = make(map[floodKey]bool)
		f.seen[nodeAddr] = m
	}
	m[key] = true
}

func (f *floodSim) isSeen(nodeAddr uint32, key floodKey) bool {
	return f.seen[nodeAddr][key]
}

// noteHeardDuplicate cancels nodeAddr's own pending rebroadcast for key once
// floodSuppressAfterHeard other relays of it have been overheard (Meshtastic's
// duplicate-suppression rule).
func (f *floodSim) noteHeardDuplicate(nodeAddr uint32, key floodKey) {
	nm := f.pending[nodeAddr]
	if nm == nil {
		return
	}
	p, ok := nm[key]
	if !ok || p.canceled {
		return
	}
	p.heard++
	if p.heard >= floodSuppressAfterHeard {
		p.canceled = true
	}
}

func avgUs(xs []uint64) float64 {
	if len(xs) == 0 {
		return 0
	}
	var sum uint64
	for _, x := range xs {
		sum += x
	}
	return float64(sum) / float64(len(xs))
}

// --- Scenario-level routing-mode selection ---
//
// Read directly off the scenario JSON file with encoding/json, independent
// of and in addition to the C side's own cJSON parse (sim_scenario.c has no
// "routing" field): keeps the flood mode entirely Go-side. Unknown fields
// are ignored by both parsers, so this is a strictly additive,
// backward-compatible scenario schema extension.

type routingConfigJSON struct {
	Routing       string `json:"routing"`
	FloodHopLimit *int   `json:"flood_hop_limit"`
}

// loadRoutingConfig reads the scenario bytes' optional "routing" ("reactive",
// the default, or "flood") and "flood_hop_limit" fields. Any parse failure or
// unrecognized value falls back to "reactive", the behavior for any scenario
// that does not explicitly opt into flood mode, so loading never silently
// changes an existing scenario's interpretation.
func loadRoutingConfig(data []byte) (mode string, hopLimit byte) {
	hopLimit = floodDefaultHopLimit
	var cfg routingConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return "reactive", hopLimit
	}
	mode = strings.ToLower(strings.TrimSpace(cfg.Routing))
	if mode != "flood" {
		mode = "reactive"
	}
	if cfg.FloodHopLimit != nil && *cfg.FloodHopLimit > 0 && *cfg.FloodHopLimit <= 255 {
		hopLimit = byte(*cfg.FloodHopLimit)
	}
	return mode, hopLimit
}

// intermediateRREPConfigJSON is the scenario-level A/B switch for
// intermediate-node RREP (see bridge.h's bridge_set_intermediate_rrep_enabled
// doc comment): a pointer so "field omitted" is distinguishable from "field
// explicitly false".
type intermediateRREPConfigJSON struct {
	IntermediateRREP *bool `json:"intermediate_rrep"`
}

// loadIntermediateRREPConfig reads the scenario bytes' optional
// "intermediate_rrep" field (default true, matching firmware's always-on
// shipped behavior). Any parse failure or omitted field returns true, same
// fail-open-to-today's-default convention as loadRoutingConfig.
func loadIntermediateRREPConfig(data []byte) bool {
	var cfg intermediateRREPConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return true
	}
	if cfg.IntermediateRREP == nil {
		return true
	}
	return *cfg.IntermediateRREP
}

// floodTransportConfigJSON is the scenario-level switch for the REAL
// firmware flood transport (main/mesh_task.c's s_flood_transport,
// driven here through bridge_set_flood_transport_enabled), read the same way
// as intermediateRREPConfigJSON above: a pointer so "field omitted" (default
// false, matching firmware's shipped NVS default) is distinguishable from
// "explicitly false". This is unrelated to routingConfigJSON's "routing":
// "flood" above (that selects the Go-only floodSim MODEL used as a
// benchmark); "flood_transport" instead makes bridge.c's normal packet path
// call the real firmware flood decide for unicast DATA.
type floodTransportConfigJSON struct {
	FloodTransport *bool `json:"flood_transport"`
	FloodHopLimit  *int  `json:"flood_hop_limit"`
}

// floodTransportDefaultHopLimit mirrors firmware's FLOOD_HOP_LIMIT_DEFAULT (8),
// the shipped flood-transport origination hop budget. Distinct from
// floodDefaultHopLimit (3) above, which is the Go-only floodSim MODEL's
// Meshtastic-style default. Kept as a plain Go const so this loader stays
// cgo-free; the C side clamps the value it is handed.
const floodTransportDefaultHopLimit = 8

// loadFloodTransportConfig reads the scenario bytes' optional "flood_transport"
// field (default false, matching s_flood_transport's shipped NVS default) and
// the optional "flood_hop_limit" field (default floodTransportDefaultHopLimit,
// matching firmware's s_flood_hop_limit default). Any parse failure or
// omitted field returns those defaults, the same fail-open-to-today's-default
// convention as loadRoutingConfig / loadIntermediateRREPConfig. The hop limit
// is returned unclamped; bridge_set_flood_hop_limit clamps it to the firmware
// range, so operators/tests can sweep it to match a network's diameter.
func loadFloodTransportConfig(data []byte) (transport bool, hopLimit int) {
	hopLimit = floodTransportDefaultHopLimit
	var cfg floodTransportConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return false, hopLimit
	}
	if cfg.FloodHopLimit != nil {
		hopLimit = *cfg.FloodHopLimit
	}
	if cfg.FloodTransport == nil {
		return false, hopLimit
	}
	return *cfg.FloodTransport, hopLimit
}

// --- Event handlers (dispatched from sim.go's dispatchEvent when
// s.routingMode == "flood") ---

// handleGenerateMessageFlood originates one scripted message as a flooded,
// hop-limited broadcast: no discovery, no route, no unicast next-hop, exact
// same scripted src/dest pairing the reactive path receives for this event.
func (s *Sim) handleGenerateMessageFlood(evt *C.sim_event_t) {
	nd := C.bridge_get_node_event(evt)
	nodeID := C.GoString(&nd.node_id[0])
	destAddr := uint32(nd.addr)
	ts := getEventTimestamp(evt)

	node := nodeArrayFindByID(&s.nodes, nodeID)
	if node == nil || !bool(node.active) {
		return
	}

	fl := s.flood
	fl.totalScripted++

	packetID := uint32(C.pcg32_random(&s.rng))
	srcAddr := uint32(node.addr)
	fl.origins[packetID] = &floodOrigin{srcAddr: srcAddr, destAddr: destAddr, sentUs: ts}

	// A node never relays its own origination if it hears its own broadcast
	// echoed back by a neighbor (mirrors the reactive path's is_own_echo
	// guard): pre-seed the dedup set.
	key := floodKey{msgType: floodMsgData, packetID: packetID, srcAddr: srcAddr}
	fl.markSeen(srcAddr, key)

	frame := encodeFloodFrame(floodMsgData, fl.hopLimit, packetID, srcAddr, destAddr, 0)
	C.metrics_record_message_sent(&s.metrics)
	if s.floodTransmit(node, frame, C.uint8_t(C.PKT_TYPE_DATA), ts) {
		fl.dataTx++
		s.emitJSON(map[string]any{
			"type": "flood_sent", "timestamp_us": ts, "node": nodeID,
			"dest": fmt.Sprintf("0x%08X", destAddr), "packet_id": fmt.Sprintf("0x%08X", packetID),
		})
	}
}

// handleReceivePacketFlood is flood mode's whole receive-side protocol: no
// bramble_header_t, no routing table lookups, no reverse routes -- decode
// the flood.go wire frame, dedup by (packet_id, src_addr) at THIS node, and
// either note a heard duplicate (possibly canceling our own pending
// rebroadcast) or process it as new: deliver-if-addressed-to-us, then
// schedule a possible SNR-delayed rebroadcast if hop_limit remains.
func (s *Sim) handleReceivePacketFlood(evt *C.sim_event_t) {
	pkt := C.bridge_get_packet_event(evt)
	rx := C.node_array_find_by_addr(&s.nodes, pkt.dest_addr)
	if rx == nil || !bool(rx.active) {
		return
	}
	if int(pkt.len) < floodFrameSize {
		return
	}
	raw := C.GoBytes(unsafe.Pointer(&pkt.data[0]), C.int(pkt.len))
	f, ok := decodeFloodFrame(raw)
	if !ok {
		return
	}

	fl := s.flood
	rxAddr := uint32(rx.addr)
	key := floodKey{msgType: f.msgType, packetID: f.packetID, srcAddr: f.srcAddr}
	ts := getEventTimestamp(evt)

	if fl.isSeen(rxAddr, key) {
		// Already processed this exact flood at this node (fan-out from
		// multiple neighbors, or hearing our own rebroadcast come back):
		// note it as an overheard duplicate so a still-pending rebroadcast
		// of ours can be suppressed, exactly Meshtastic's contention rule.
		fl.noteHeardDuplicate(rxAddr, key)
		return
	}
	fl.markSeen(rxAddr, key)

	// Self-echo guard: a node hearing its own origination/ack reflected
	// back by a relay is not a new event for it.
	if f.srcAddr != rxAddr {
		switch f.msgType {
		case floodMsgData:
			if f.destAddr == rxAddr {
				s.floodOnDataReached(rx, f, ts)
			}
		case floodMsgAck:
			if f.destAddr == rxAddr {
				s.floodOnAckReached(rx, f, ts)
			}
		}
	}

	if f.hopLimit > 0 {
		s.floodScheduleRelay(rx, f, int8(pkt.snr), ts)
	}
}

// floodOnDataReached fires the LOOSE delivery bar (destination reach) the
// first time destAddr's node receives this DATA packet_id, and originates a
// Meshtastic-style flooded ACK back toward the true sender (also hop-
// limited, also flooded, no reverse route) so the STRICT bar has a genuine
// signal instead of being N/A.
func (s *Sim) floodOnDataReached(rx *C.sim_node_t, f floodFrame, ts uint64) {
	fl := s.flood
	o, ok := fl.origins[f.packetID]
	if !ok || o.reached {
		return
	}
	o.reached = true
	o.reachedUs = ts
	fl.reachedCount++
	fl.reachedLatUs = append(fl.reachedLatUs, ts-o.sentUs)

	rxAddr := uint32(rx.addr)
	s.emitJSON(map[string]any{
		"type": "flood_reached", "timestamp_us": ts,
		"node": C.GoString(&rx.id[0]), "packet_id": fmt.Sprintf("0x%08X", f.packetID),
	})

	ackID := uint32(C.pcg32_random(&s.rng))
	ackKey := floodKey{msgType: floodMsgAck, packetID: ackID, srcAddr: rxAddr}
	fl.markSeen(rxAddr, ackKey)

	ackFrame := encodeFloodFrame(floodMsgAck, fl.hopLimit, ackID, rxAddr, o.srcAddr, f.packetID)
	if s.floodTransmit(rx, ackFrame, C.uint8_t(C.PKT_TYPE_ACK), ts) {
		fl.ackTx++
	}
}

// floodOnAckReached fires the STRICT delivery-with-confirmation bar: the
// true original sender of the DATA received a flooded ACK addressed to it.
func (s *Sim) floodOnAckReached(rx *C.sim_node_t, f floodFrame, ts uint64) {
	fl := s.flood
	o, ok := fl.origins[f.corrID]
	if !ok || o.confirmed || !o.reached {
		return
	}
	rxAddr := uint32(rx.addr)
	if rxAddr != o.srcAddr {
		return
	}
	o.confirmed = true
	o.confirmedUs = ts
	fl.confirmedCount++
	fl.confirmedLatUs = append(fl.confirmedLatUs, ts-o.sentUs)

	s.emitJSON(map[string]any{
		"type": "flood_confirmed", "timestamp_us": ts,
		"node": C.GoString(&rx.id[0]), "packet_id": fmt.Sprintf("0x%08X", f.corrID),
	})
}

// floodScheduleRelay schedules rx's possible rebroadcast of f, hop_limit
// decremented, after an SNR-derived delay: weaker snr (the reception that
// just happened) -> shorter delay, so the hearer least likely to have
// redundant coverage tends to key up first. A later overheard duplicate
// before the delay elapses can cancel it (floodSim.noteHeardDuplicate).
func (s *Sim) floodScheduleRelay(rx *C.sim_node_t, f floodFrame, snr int8, ts uint64) {
	fl := s.flood
	rxAddr := uint32(rx.addr)
	newFrame := encodeFloodFrame(f.msgType, f.hopLimit-1, f.packetID, f.srcAddr, f.destAddr, f.corrID)

	key := floodKey{msgType: f.msgType, packetID: f.packetID, srcAddr: f.srcAddr}
	pktType := C.uint8_t(C.PKT_TYPE_DATA)
	if f.msgType == floodMsgAck {
		pktType = C.uint8_t(C.PKT_TYPE_ACK)
	}

	nm := fl.pending[rxAddr]
	if nm == nil {
		nm = make(map[floodKey]*floodPending)
		fl.pending[rxAddr] = nm
	}
	nm[key] = &floodPending{frame: newFrame, pktType: pktType}

	delayUs := uint64(floodRebroadcastDelayMs(int(snr), &s.rng)) * 1000
	cFrame := (*C.uint8_t)(unsafe.Pointer(&newFrame[0]))
	relayEvt := C.bridge_make_flood_relay_event(C.uint64_t(ts+delayUs), C.uint32_t(rxAddr), cFrame,
		C.uint16_t(len(newFrame)))
	eventQueuePush(&s.events, &relayEvt)
}

// floodRebroadcastDelayMs: documented approximation of Meshtastic's SNR-
// based contention window. snr is clamped to [0, floodRebroadcastMaxSNRdB];
// a small random component (0-49ms) breaks exact ties between nodes at
// identical SNR.
func floodRebroadcastDelayMs(snr int, rng *C.pcg32_state_t) int {
	if snr < 0 {
		snr = 0
	}
	if snr > floodRebroadcastMaxSNRdB {
		snr = floodRebroadcastMaxSNRdB
	}
	frac := float64(snr) / float64(floodRebroadcastMaxSNRdB)
	delay := floodRebroadcastBaseMs + int(frac*floodRebroadcastSpreadMs)
	jitter := int(uint32(C.pcg32_random(rng)) % 50)
	return delay + jitter
}

// handleFloodRelayDue fires when a scheduled rebroadcast (floodScheduleRelay)
// comes due: the real airtime budget gets the final say (a node that had
// budget when it scheduled the relay but has since spent it still yields),
// and a canceled or already-fired entry (see floodSim.noteHeardDuplicate)
// is silently skipped.
func (s *Sim) handleFloodRelayDue(evt *C.sim_event_t) {
	pkt := C.bridge_get_packet_event(evt)
	nodeAddr := uint32(pkt.src_addr)

	fl := s.flood
	nm := fl.pending[nodeAddr]
	if nm == nil || int(pkt.len) < floodFrameSize {
		return
	}
	raw := C.GoBytes(unsafe.Pointer(&pkt.data[0]), C.int(pkt.len))
	f, ok := decodeFloodFrame(raw)
	if !ok {
		return
	}
	key := floodKey{msgType: f.msgType, packetID: f.packetID, srcAddr: f.srcAddr}
	p, ok := nm[key]
	if !ok {
		return
	}
	delete(nm, key)
	if p.canceled {
		fl.relaysCanceled++
		return
	}

	node := C.node_array_find_by_addr(&s.nodes, C.uint32_t(nodeAddr))
	if node == nil || !bool(node.active) {
		return
	}
	ts := getEventTimestamp(evt)
	if s.floodTransmit(node, p.frame, p.pktType, ts) {
		fl.relaysFired++
		if p.pktType == C.uint8_t(C.PKT_TYPE_DATA) {
			fl.dataTx++
		} else {
			fl.ackTx++
		}
	}
}

// floodTransmit budget-gates a flood frame through the SAME real airtime
// budget the reactive path's budget_gated_send (bridge.c) uses -- BROADCAST
// tier, since every flood frame is a physical broadcast -- then hands it to
// the same C.sim_radio_broadcast chokepoint every other TX site in this sim
// converges through, so collision/capture/ToA/per-type airtime accounting
// are identical regardless of routing mode. Returns false (packet dropped,
// no queue) on budget denial, matching every other gated TX site's
// semantics.
func (s *Sim) floodTransmit(node *C.sim_node_t, frame []byte, pktType C.uint8_t, ts uint64) bool {
	var pkt C.outbound_packet_t
	pkt.len = C.uint16_t(len(frame))
	pkt.is_broadcast = C.bool(true)
	pkt.dest_addr = C.uint32_t(0xFFFFFFFF)
	pkt.pkt_type = pktType
	for i, b := range frame {
		pkt.data[i] = C.uint8_t(b)
	}

	airtimeMs := C.radio_frame_airtime_ms(&s.radio, C.uint16_t(len(frame)))
	if !bool(C.airtime_budget_can_transmit(&node.airtime, C.AIRTIME_TIER_BROADCAST, airtimeMs)) {
		node.budget_denied[C.AIRTIME_IDX_BROADCAST]++
		return false
	}
	C.airtime_budget_debit(&node.airtime, C.AIRTIME_TIER_BROADCAST, airtimeMs)
	C.sim_radio_broadcast(node, &pkt, &s.nodes, &s.radio, &s.rng, &s.events, &s.metrics, C.uint64_t(ts))
	return true
}
