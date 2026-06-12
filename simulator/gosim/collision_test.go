package main

import "testing"

// The collision-model tests below drive sim_radio directly through the
// radioHarness (radio_harness.go). Default PHY mirrors the firmware's
// long-range profile: SF10, 125 kHz, CR 4/5, 22 dBm. At those settings a
// 32-byte frame has a time-on-air of 485.376 ms and the preamble
// (12 + 4.25 symbols at 8.192 ms/symbol) lasts 133.12 ms.

const (
	addrRx = 0x000000C0
	addrA  = 0x000000A0
	addrB  = 0x000000B0

	frameBytes = 32
)

// newTriangle places a receiver at the origin plus two transmitters at the
// given distances from it (along different axes so the transmitters are also
// within audible range of each other). LBT is disabled: these tests validate
// the raw overlap/capture/half-duplex engine; LBT has its own tests.
func newTriangle(distA, distB float32) *radioHarness {
	h := newRadioHarness()
	h.disableLBT()
	h.addNode(addrRx, 0, 0)
	h.addNode(addrA, distA, 0)
	h.addNode(addrB, 0, distB)
	return h
}

func outcomeOf(t *testing.T, results []rxResult, src uint32) int {
	t.Helper()
	for _, r := range results {
		if r.srcAddr == src && r.destAddr == addrRx {
			return r.outcome
		}
	}
	t.Fatalf("no reception from 0x%08X at receiver", src)
	return -1
}

func TestTimeOnAirMatchesSemtechFormula(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	// SF10, BW 125 kHz, CR 4/5, explicit header, CRC on, 12-symbol preamble:
	// t_sym = 2^10/125000 = 8.192 ms
	// preamble = (12 + 4.25) * 8.192 = 133.120 ms
	// payload symbols = 8 + ceil((8*32 - 4*10 + 28 + 16) / (4*10)) * 5 = 43
	// payload = 43 * 8.192 = 352.256 ms; total = 485.376 ms
	if got := h.frameAirtimeUs(frameBytes); got != 485376 {
		t.Fatalf("ToA(32B, SF10/125k/CR4-5) = %d us, want 485376", got)
	}
	if got := h.preambleUs(); got != 133120 {
		t.Fatalf("preamble = %d us, want 133120", got)
	}
}

func TestRSSILogDistancePathLoss(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	// RSSI(d) = 22 dBm - 52 dB - 10*2.9*log10(d), d in grid units (10 m).
	cases := []struct {
		dist float32
		want int
	}{
		{1, -30},   // at d0
		{10, -59},  // -29 dB/decade
		{100, -88}, // two decades
		{150, -93}, // default range edge
	}
	for _, c := range cases {
		if got := h.rssiAt(c.dist); got != c.want {
			t.Errorf("rssi(%v) = %d, want %d", c.dist, got, c.want)
		}
	}
}

func TestOverlappingEqualPowerTransmissionsCollide(t *testing.T) {
	h := newTriangle(100, 100) // equal distance: equal RSSI, no capture
	defer h.free()

	toa := uint64(h.frameAirtimeUs(frameBytes))
	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrB, addrRx, frameBytes, toa/2) // overlaps A's second half

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeCollision {
		t.Errorf("A outcome = %d, want collision", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeCollision {
		t.Errorf("B outcome = %d, want collision", got)
	}
}

func TestNonOverlappingTransmissionsBothPass(t *testing.T) {
	h := newTriangle(100, 100)
	defer h.free()

	toa := uint64(h.frameAirtimeUs(frameBytes))
	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrB, addrRx, frameBytes, toa+1000) // 1 ms after A ends

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeOK {
		t.Errorf("A outcome = %d, want ok", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeOK {
		t.Errorf("B outcome = %d, want ok", got)
	}
}

func TestCaptureStrongerFirstPacketSurvives(t *testing.T) {
	// A at 20 units (-67.7 dBm), B at 100 units (-88 dBm): A is ~20 dB
	// stronger, well past the 6 dB capture threshold.
	h := newTriangle(20, 100)
	defer h.free()

	toa := uint64(h.frameAirtimeUs(frameBytes))
	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrB, addrRx, frameBytes, toa/2)

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeCaptured {
		t.Errorf("strong A outcome = %d, want captured", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeCollision {
		t.Errorf("weak B outcome = %d, want collision", got)
	}
}

func TestCaptureStrongerLateWithinPreambleSurvives(t *testing.T) {
	h := newTriangle(20, 100)
	defer h.free()

	// B (weak) first; A (strong) starts 100 ms in, still inside B's
	// 133.12 ms preamble: the receiver re-syncs to A.
	h.transmit(addrB, addrRx, frameBytes, 0)
	h.transmit(addrA, addrRx, frameBytes, 100000)

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeCaptured {
		t.Errorf("strong late-in-preamble A outcome = %d, want captured", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeCollision {
		t.Errorf("weak B outcome = %d, want collision", got)
	}
}

func TestCaptureStrongerLateAfterPreambleBothLost(t *testing.T) {
	h := newTriangle(20, 100)
	defer h.free()

	// A (strong) starts 200 ms in, past B's preamble: the receiver is locked
	// onto B's payload, cannot re-sync, and B is trampled. Both lost.
	h.transmit(addrB, addrRx, frameBytes, 0)
	h.transmit(addrA, addrRx, frameBytes, 200000)

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeCollision {
		t.Errorf("strong post-preamble A outcome = %d, want collision", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeCollision {
		t.Errorf("weak B outcome = %d, want collision", got)
	}
}

func TestHalfDuplexReceiverTransmittingMissesPacket(t *testing.T) {
	h := newTriangle(100, 100)
	defer h.free()

	// Receiver broadcasts at t=0 (on air for one full ToA); A's packet to it
	// starts 100 ms in and is missed because the radio was transmitting.
	h.transmit(addrRx, 0xFFFFFFFF, frameBytes, 0)
	h.transmit(addrA, addrRx, frameBytes, 100000)

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeHalfDuplex {
		t.Errorf("A outcome = %d, want half-duplex drop", got)
	}
}

func TestHalfDuplexSerializesOwnTransmissions(t *testing.T) {
	h := newTriangle(100, 100)
	defer h.free()

	// Two back-to-back sends from A at the same instant: the radio cannot
	// transmit both at once, so the second is queued behind the first and
	// both arrive intact.
	toa := uint64(h.frameAirtimeUs(frameBytes))
	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrA, addrRx, frameBytes, 0)

	res := h.receptions()
	var got []rxResult
	for _, r := range res {
		if r.srcAddr == addrA && r.destAddr == addrRx {
			got = append(got, r)
		}
	}
	if len(got) != 2 {
		t.Fatalf("expected 2 receptions from A, got %d", len(got))
	}
	for i, r := range got {
		if r.outcome != rxOutcomeOK {
			t.Errorf("reception %d outcome = %d, want ok", i, r.outcome)
		}
	}
	d := got[1].tsUs - got[0].tsUs
	if d != toa {
		t.Errorf("second reception %d us after first, want exactly one ToA (%d us)", d, toa)
	}
}

func TestLBTDefersTransmissionAndAvoidsCollision(t *testing.T) {
	// SF7 / 250 kHz: 32-byte frame ToA is ~36 ms, well under the minimum
	// 50 ms LBT backoff, so one deferral always clears the channel.
	h := newRadioHarness()
	defer h.free()
	h.setPHY(7, 250000, 1)
	h.addNode(addrRx, 0, 0)
	h.addNode(addrA, 100, 0)
	h.addNode(addrB, 0, 100) // 141 units from A: hears A's carrier

	toa := uint64(h.frameAirtimeUs(frameBytes))
	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrB, addrRx, frameBytes, toa/3) // mid-frame: CAD detects A

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeOK {
		t.Errorf("A outcome = %d, want ok", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeOK {
		t.Errorf("B outcome = %d, want ok (deferred past A by LBT)", got)
	}
	if got := h.lbtBackoffs(); got != 1 {
		t.Errorf("lbt_backoffs = %d, want 1", got)
	}
	// B's reception must land after A's frame ended
	for _, r := range res {
		if r.srcAddr == addrB && r.tsUs < toa {
			t.Errorf("B reception at %d us, before A's frame ended (%d us)", r.tsUs, toa)
		}
	}
}

func TestLBTTransmitsAnywayAfterMaxAttempts(t *testing.T) {
	// At SF10 a 32-byte frame lasts 485 ms; B's CAD starts 10 ms in, and
	// even after backoffs the channel may still be busy. The firmware (and
	// the model) transmits anyway after 3 attempts: no starvation.
	h := newRadioHarness()
	defer h.free()
	h.addNode(addrRx, 0, 0)
	h.addNode(addrA, 100, 0)
	h.addNode(addrB, 0, 100)

	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrB, addrRx, frameBytes, 10000)

	res := h.receptions()
	var fromB int
	for _, r := range res {
		if r.srcAddr == addrB && r.destAddr == addrRx {
			fromB++
		}
	}
	if fromB != 1 {
		t.Errorf("expected exactly 1 transmission from B despite busy channel, got %d", fromB)
	}
	if got := h.lbtBackoffs(); got < 1 {
		t.Errorf("lbt_backoffs = %d, want >= 1", got)
	}
}

func TestCollisionsDisabledOverlapPasses(t *testing.T) {
	h := newTriangle(100, 100)
	defer h.free()
	h.disableCollisions()

	toa := uint64(h.frameAirtimeUs(frameBytes))
	h.transmit(addrA, addrRx, frameBytes, 0)
	h.transmit(addrB, addrRx, frameBytes, toa/2)

	res := h.receptions()
	if got := outcomeOf(t, res, addrA); got != rxOutcomeOK {
		t.Errorf("A outcome = %d, want ok with collisions disabled", got)
	}
	if got := outcomeOf(t, res, addrB); got != rxOutcomeOK {
		t.Errorf("B outcome = %d, want ok with collisions disabled", got)
	}
}
