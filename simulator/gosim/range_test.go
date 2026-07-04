package main

import "testing"

// Reception range now derives from the LoRa link budget (radio_sensitivity_dbm
// + radio_derive_range in sim_radio.c) instead of being a fixed disk
// independent of SF/BW. These tests calibrate and sanity-check that model:
// SF10/125 kHz (the firmware's default profile) must reproduce the
// simulator's long-standing ~150-unit baseline range, and other SF/BW
// combinations must move relative to that baseline the way a real link
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

func TestSF10BaselineRangeCalibration(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	// newRadioHarness -> radio_config_init leaves the firmware default PHY
	// (SF10, 125 kHz) in place, so radio.range is already the derived value.
	got := h.rangeField()
	approxEqual(t, got, 150.0, 2.0, "default (SF10/125k) derived range")

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

	// Calibration + datasheet-scale sanity checks.
	approxEqual(t, sf10_125, 150.0, 2.0, "range(SF10,125k)")
	approxEqual(t, sf12_125, 223.0, 5.0, "range(SF12,125k)")
	// SF7/250k should be roughly half of the SF10/125k baseline (the ~12 dB
	// SF+BW budget difference over path_loss_exp 2.9); the exact datasheet
	// deltas (9 dB SF7->SF10, 3.01 dB per BW octave) land it at ~58 units.
	approxEqual(t, sf7_250, 58.0, 5.0, "range(SF7,250k)")
	if sf7_250 >= sf10_125/2.0+10.0 {
		t.Errorf("range(SF7,250k) = %v not meaningfully shorter than half of range(SF10,125k) = %v",
			sf7_250, sf10_125)
	}
}

func TestSensitivityModelDatasheetValues(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	// Bandwidth adjustment is 0 at 125 kHz, so at 125 kHz the sensitivity
	// deltas between SF must match the datasheet deltas exactly (only
	// NOISE_MARGIN_DB, a constant, is added to every one of them).
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
// budget, SF7/250k's ~58-unit range is well under 120 units, so adjacent
// grid neighbors are out of range: radio_can_receive must reject them.
// SF10/125k at the same spacing must still connect (range ~150 > 120),
// preserving the baseline.
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
	// Default PHY is SF10/125k.
	h2.addNode(0x100, 0, 0)
	h2.addNode(0x101, gridSpacing, 0)
	tx2 := h2.activateNode(0x100)
	rx2 := h2.activateNode(0x101)
	if got := h2.deriveRange(); got <= gridSpacing {
		t.Fatalf("range(SF10,125k) = %v, want > grid spacing %v (baseline preservation)",
			got, gridSpacing)
	}
	if !radioCanReceive(h2, tx2, rx2) {
		t.Errorf("SF10/125k neighbor at legacy 120-unit spacing: want receivable (baseline unchanged)")
	}
}
