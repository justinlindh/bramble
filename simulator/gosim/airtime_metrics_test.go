package main

import "testing"

// TestPerTypeAirtimeMatchesToAAndControlPctIsHonest pins the core claim:
// per-packet-type airtime accumulators charge the SAME ToA the radio
// medium model computes (bramble_calculate_airtime_us via
// radio_frame_airtime_us), and control_airtime_pct is genuinely
// ToA-weighted, not a packet-count ratio wearing an airtime label.
//
// Known mix at a 2-node scenario: 1 beacon (54B), 1 RREQ (30B), 3 DATA
// frames (200B each). By packet COUNT, control (beacon+RREQ) is 2 of 5
// packets = 40%. But the 200B DATA frames carry far more real time-on-air
// than the tiny 54B/30B control frames, so the ToA-weighted share must be
// well under 40%: this is what distinguishes a genuine ToA-weighted metric
// from a relabeled packet-count ratio.
func TestPerTypeAirtimeMatchesToAAndControlPctIsHonest(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addrA = 0x000000F0
	const addrB = 0x000000F1
	h.addNode(addrA, 0, 0)
	h.addNode(addrB, 10, 0)

	now := uint64(0)
	beaconLen := beaconWireSize()
	const rreqLen = 30
	const dataLen = 200

	h.transmitTyped(addrA, 0xFFFFFFFF, beaconLen, pktTypeBeacon, now)
	now += 1000
	h.transmitTyped(addrA, 0xFFFFFFFF, rreqLen, pktTypeRREQ, now)
	now += 1000
	for i := 0; i < 3; i++ {
		h.transmitTyped(addrA, addrB, dataLen, pktTypeData, now)
		now += 1000
	}

	wantBeaconUs := uint64(h.frameAirtimeUs(beaconLen))
	wantRREQUs := uint64(h.frameAirtimeUs(rreqLen))
	wantDataUs := uint64(h.frameAirtimeUs(dataLen)) * 3

	if got := h.airtimeUsByType(metricBeacon); got != wantBeaconUs {
		t.Errorf("beacon airtime_us_by_type = %d, want %d (radio_frame_airtime_us)",
			got, wantBeaconUs)
	}
	if got := h.airtimeUsByType(metricRREQ); got != wantRREQUs {
		t.Errorf("rreq airtime_us_by_type = %d, want %d", got, wantRREQUs)
	}
	if got := h.airtimeUsByType(metricData); got != wantDataUs {
		t.Errorf("data airtime_us_by_type = %d, want %d (3 frames)", got, wantDataUs)
	}
	// Untouched buckets must stay zero: this mix has no RREP/RERR/ACK/receipt.
	if got := h.airtimeUsByType(metricRREP); got != 0 {
		t.Errorf("rrep airtime_us_by_type = %d, want 0 (none sent)", got)
	}

	packetPct := h.controlPacketPct()
	airtimePct := h.controlAirtimePct()

	if packetPct != 40.0 {
		t.Fatalf("control_packet_pct = %v, want 40 (2 control / 5 total packets by count)",
			packetPct)
	}
	if airtimePct <= 0 || airtimePct >= packetPct {
		t.Fatalf("control_airtime_pct = %v, want strictly between 0 and control_packet_pct (%v): "+
			"the tiny beacon+RREQ frames should be a much smaller share of ToA than of packet count",
			airtimePct, packetPct)
	}
}
