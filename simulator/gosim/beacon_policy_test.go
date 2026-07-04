package main

import "testing"

// TestBeaconPolicyFixedIgnoresNeighborCount proves Task 3's default: the
// sim's beacon cadence, unless a scenario opts into "beacon.adaptive": true,
// comes from the REAL firmware function beacon_interval_decide()
// (main/beacon_policy_calc.c) running in fixed mode, which returns the
// configured interval_ms regardless of mesh conditions. This mirrors
// firmware's shipped default (BEACON_MODE_FIXED, mesh_task.c:298-306): a
// dense neighbor table must NOT stretch the interval.
func TestBeaconPolicyFixedIgnoresNeighborCount(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addr = 0x000000E0
	h.addNode(addr, 0, 0)
	n := h.activateNode(addr)
	// h.beacon.adaptive is false by default (sim_beacon_policy_init).

	// Dense enough to hit the adaptive policy's max-interval branch if
	// adaptive were mistakenly active; fixed mode must ignore this.
	h.setNeighborCount(n, 20, 0)
	h.forceBeaconDue(n, 0)

	if got := h.tick(n, 0); got != 1 {
		t.Fatalf("expected exactly 1 beacon on the forced-due tick, got %d", got)
	}

	interval := h.nextBeaconDue(n) // started the wait at t=0
	const base = 60_000_000        // firmware default interval_ms=60000
	const jitter = 5_000_000       // +-5s BEACON_JITTER_MS
	if interval < base-jitter || interval >= base+jitter {
		t.Fatalf("fixed-mode interval = %d us, want %d +-%d us regardless of a dense mesh",
			interval, base, jitter)
	}
}

// TestBeaconPolicyAdaptiveStretchesOnDenseMesh proves the opt-in path:
// with beacon.adaptive=true and the real neighbor table crossing the dense
// threshold (>=10, firmware's default dense_threshold), beacon_interval_decide
// must stretch the interval toward max_interval_ms (120s default), exactly
// as firmware's adaptive policy does when a mesh gets crowded.
func TestBeaconPolicyAdaptiveStretchesOnDenseMesh(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addr = 0x000000E1
	h.addNode(addr, 0, 0)
	n := h.activateNode(addr)
	h.setBeaconAdaptive(true)

	// First beacon: sparse mesh, forced due at t=0. Base interval expected.
	h.setNeighborCount(n, 3, 0)
	h.forceBeaconDue(n, 0)
	if got := h.tick(n, 0); got != 1 {
		t.Fatalf("expected exactly 1 beacon on the forced-due tick, got %d", got)
	}
	sparseDue := h.nextBeaconDue(n)
	const base = 60_000_000
	const jitter = 5_000_000
	if sparseDue < base-jitter || sparseDue >= base+jitter {
		t.Fatalf("sparse-mesh interval = %d us, want %d +-%d us", sparseDue, base, jitter)
	}

	// Cross the dense threshold before the next scheduled beacon fires.
	h.setNeighborCount(n, 12, sparseDue)
	if got := h.tick(n, sparseDue); got != 1 {
		t.Fatalf("expected exactly 1 beacon on the second forced-due tick, got %d", got)
	}
	denseInterval := h.nextBeaconDue(n) - sparseDue

	const max = 120_000_000 // firmware default max_interval_ms=120000
	if denseInterval < max-jitter || denseInterval >= max+jitter {
		t.Fatalf("dense-mesh interval = %d us, want %d +-%d us (stretched to max_interval_ms)",
			denseInterval, max, jitter)
	}
}
