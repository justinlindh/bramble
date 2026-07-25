package main

import "testing"

// Reception range now derives from the LoRa link budget (radio_sensitivity_dbm
// + radio_derive_range in sim_radio.c) instead of being a fixed disk
// independent of SF/BW. These tests calibrate and sanity-check that model: the
// firmware's default PHY (the frequency plan's default_sf/default_bw_hz, which
// mesh_init_radio_config programs over the radio profile's values) must
// reproduce the simulator's long-standing ~150-unit baseline range, and other
// SF/BW combinations must move relative to that baseline the way a real link
// budget would (higher SF = more sensitive = longer range; wider bandwidth =
// higher noise floor = shorter range).

func approxEqual(t *testing.T, got, want, tolerance float32, what string) {
	t.Helper()
	diff := got - want
	if diff < 0 {
		diff = -diff
	}
	if diff > tolerance {
		t.Errorf("%s = %v, want %v +/- %v", what, got, want, tolerance)
	}
}

// TestDefaultPHYIsFrequencyPlanDefault pins the property this model got wrong
// for as long as it had a default PHY: the medium must be modeled at the PHY
// the firmware transmits at, which is the frequency plan's, not the radio
// profile table's. mesh_task.c's mesh_init_radio_config loads
// RADIO_PROFILE_LONG_RANGE (SF10) and then overwrites sf/bw_hz with
// freq_plan_get_default()'s (SF9/125 kHz on every shipped plan), so a real
// node's boot log reads "SF9 BW125000". Comparing radio_config_init's output
// against the plan table directly is what keeps the two coupled: change a
// plan's default_sf and either the model follows or this test fails.
func TestDefaultPHYIsFrequencyPlanDefault(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	wantSF, wantBW := planDefaultPHY()
	gotSF, gotBW := h.phy()
	if gotSF != wantSF || gotBW != wantBW {
		t.Errorf("radio_config_init PHY = SF%d/%d Hz, want the frequency plan's SF%d/%d Hz",
			gotSF, gotBW, wantSF, wantBW)
	}
	// Guard against the plan and the profile table silently converging, which
	// would make the assertion above pass for the wrong reason.
	if wantSF == 10 {
		t.Errorf("frequency plan default_sf = 10, the same as RADIO_PROFILE_LONG_RANGE; "+
			"this test can no longer distinguish the plan default from the profile default "+
			"(plan bw_hz = %d)", wantBW)
	}
}

// TestDefaultPHYFrameAirtime prices a frame at the default PHY and checks it
// against the firmware's own ToA function at the plan's SF, the number the
// whole channel model (offered load, collisions, LBT) is built on. At SF10 a
// 60-byte frame was charged about 732 ms; the firmware's real SF9 is about half
// that, and every published offered-load figure moved with it.
func TestDefaultPHYFrameAirtime(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	sf, bw := h.phy()
	got := h.frameAirtimeUs(60)
	// Semtech AN1200.13 for 60 bytes at SF9/125 kHz/CR 4/5, the shipped plans'
	// PHY, worked independently of bramble_calculate_airtime_us:
	//   Tsym = 2^9 / 125000 = 4.096 ms
	//   preamble = (12 + 4.25) * 4.096 ms = 66.56 ms
	//   payload symbols = 8 + ceil((8*60 - 4*9 + 28 + 16) / (4*9)) * 5
	//                   = 8 + ceil(488/36) * 5 = 8 + 14*5 = 78
	//   payload = 78 * 4.096 ms = 319.488 ms, total 386.048 ms.
	// The same frame at SF10 is 731.136 ms, so the corrected PHY charges 53% of
	// what this model used to charge every frame.
	approxEqual(t, float32(got), 386048.0, 1.0, "60-byte ToA at the default PHY (us)")

	// The correction's direction and magnitude, asserted rather than assumed:
	// the default PHY must be materially cheaper than the SF10 the model used
	// to assume, and not by a rounding error.
	h10 := newRadioHarness()
	defer h10.free()
	h10.setPHY(10, 125000, 1)
	sf10 := h10.frameAirtimeUs(60)
	if got >= sf10*3/4 {
		t.Errorf("60-byte frame at SF%d/%d Hz = %d us, want well under the SF10 cost %d us",
			sf, bw, got, sf10)
	}
}

func TestDefaultPHYBaselineRangeCalibration(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	// newRadioHarness -> radio_config_init leaves the firmware's default PHY in
	// place, so radio.range is already the derived value. The noise-margin
	// anchor (radio_noise_margin_db) is computed from that same PHY, so this
	// 150-unit baseline holds whatever the frequency plan's default SF is:
	// correcting the modeled SF changes airtime without moving the topology of
	// any scenario that lets range derive.
	got := h.rangeField()
	approxEqual(t, got, 150.0, 2.0, "default-PHY derived range")

	// radio_derive_range must agree with the field radio_config_init set.
	approxEqual(t, h.deriveRange(), got, 0.01, "radio_derive_range vs radio.range")
}

func TestDerivedRangeOrdering(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	rangeFor := func(sf, bwHz int) float32 {
		h.setPHY(sf, bwHz, 1)
		return h.deriveRange()
	}

	sf7_125 := rangeFor(7, 125000)
	sf7_250 := rangeFor(7, 250000)
	sf10_125 := rangeFor(10, 125000)
	sf10_250 := rangeFor(10, 250000)
	sf12_125 := rangeFor(12, 125000)

	// SF ordering at fixed bandwidth: higher SF => more sensitive => longer
	// range.
	if !(sf12_125 > sf10_125 && sf10_125 > sf7_125) {
		t.Errorf("SF range ordering violated: SF7=%v SF10=%v SF12=%v (want SF12 > SF10 > SF7)",
			sf7_125, sf10_125, sf12_125)
	}

	// Bandwidth ordering at fixed SF: 125 kHz (lower noise floor) => longer
	// range than 250 kHz.
	if !(sf7_125 > sf7_250) {
		t.Errorf("BW range ordering violated at SF7: 125k=%v 250k=%v (want 125k > 250k)",
			sf7_125, sf7_250)
	}
	if !(sf10_125 > sf10_250) {
		t.Errorf("BW range ordering violated at SF10: 125k=%v 250k=%v (want 125k > 250k)",
			sf10_125, sf10_250)
	}

	// Calibration + datasheet-scale sanity checks. The 150-unit anchor sits at
	// the default PHY (SF9/125 kHz), so SF10 is one datasheet step (3 dB) of
	// link budget ABOVE it: 150 * 10^(3/29) = ~190 units.
	sf9_125 := rangeFor(9, 125000)
	approxEqual(t, sf9_125, 150.0, 2.0, "range(SF9,125k), the default-PHY anchor")
	approxEqual(t, sf10_125, 190.3, 2.0, "range(SF10,125k)")
	approxEqual(t, sf12_125, 283.1, 5.0, "range(SF12,125k)")
	// SF7/250k should be roughly half of the SF9/125k baseline (the ~9 dB
	// SF+BW budget difference over path_loss_exp 2.9); the exact datasheet
	// deltas (6 dB SF7->SF9, 3.01 dB per BW octave) land it at ~73 units.
	approxEqual(t, sf7_250, 73.3, 5.0, "range(SF7,250k)")
	if sf7_250 >= sf9_125/2.0+15.0 {
		t.Errorf("range(SF7,250k) = %v not meaningfully shorter than half of range(SF9,125k) = %v",
			sf7_250, sf9_125)
	}
}

func TestSensitivityModelDatasheetValues(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	// Bandwidth adjustment is 0 at 125 kHz, so at 125 kHz the sensitivity
	// deltas between SF must match the datasheet deltas exactly (only the
	// noise-margin anchor, a constant, is added to every one of them).
	sf10 := h.sensitivityDbm(10, 125000)
	sf7 := h.sensitivityDbm(7, 125000)
	sf12 := h.sensitivityDbm(12, 125000)

	// Higher SF is MORE sensitive (more negative dBm), so sensitivity(SF10) <
	// sensitivity(SF7): the deltas below are negative going up in SF.
	approxEqual(t, sf7-sf10, 9.0, 0.01, "sensitivity(SF7) - sensitivity(SF10) at 125k")
	approxEqual(t, sf10-sf12, 5.0, 0.01, "sensitivity(SF10) - sensitivity(SF12) at 125k")

	// Bandwidth adjustment: 250 kHz is +3.01 dB (worse/less negative) than
	// 125 kHz at the same SF; 500 kHz is +6.02 dB.
	sf10_250 := h.sensitivityDbm(10, 250000)
	sf10_500 := h.sensitivityDbm(10, 500000)
	approxEqual(t, sf10_250-sf10, 3.0103, 0.01, "sensitivity(SF10,250k) - sensitivity(SF10,125k)")
	approxEqual(t, sf10_500-sf10, 6.0206, 0.01, "sensitivity(SF10,500k) - sensitivity(SF10,125k)")
}

// TestLegacyGridDisconnectsAtSF7_250k demonstrates the inversion this fix
// corrects: at the legacy 120-unit grid spacing (airtime-adaptive-*
// scenarios), the OLD fixed 150-unit disk let SF7/250k "reach" neighbors it
// physically could not at that SF/BW. With range now derived from the link
// budget, SF7/250k's ~73-unit range is well under 120 units, so adjacent
// grid neighbors are out of range: radio_can_receive must reject them. The
// default PHY at the same spacing must still connect (anchored range ~150 >
// 120), which is what keeps every legacy scenario's topology intact.
func TestLegacyGridDisconnectsAtSF7_250k(t *testing.T) {
	const gridSpacing = 120.0

	h := newRadioHarness()
	defer h.free()
	h.setPHY(7, 250000, 1)
	// setPHY only mutates sf/bw_hz/cr; radio.range is a cached field that a
	// real scenario load recomputes after applying overrides
	// (sim_scenario.c load_radio). Mirror that here.
	h.setRange(h.deriveRange())
	h.addNode(0x100, 0, 0)
	h.addNode(0x101, gridSpacing, 0)
	tx := h.activateNode(0x100)
	rx := h.activateNode(0x101)

	if got := h.deriveRange(); got >= gridSpacing {
		t.Fatalf("range(SF7,250k) = %v, want < grid spacing %v for this test to be meaningful",
			got, gridSpacing)
	}
	if radioCanReceive(h, tx, rx) {
		t.Errorf("SF7/250k neighbor at legacy 120-unit spacing: want out of range, got receivable")
	}

	h2 := newRadioHarness()
	defer h2.free()
	// Default PHY: the frequency plan's, anchored to the 150-unit baseline.
	h2.addNode(0x100, 0, 0)
	h2.addNode(0x101, gridSpacing, 0)
	tx2 := h2.activateNode(0x100)
	rx2 := h2.activateNode(0x101)
	if got := h2.deriveRange(); got <= gridSpacing {
		t.Fatalf("default-PHY range = %v, want > grid spacing %v (baseline preservation)",
			got, gridSpacing)
	}
	if !radioCanReceive(h2, tx2, rx2) {
		t.Errorf("default-PHY neighbor at legacy 120-unit spacing: want receivable (baseline unchanged)")
	}
}
