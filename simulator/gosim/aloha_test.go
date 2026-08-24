package main

import (
	"math"
	"math/rand"
	"sort"
	"testing"
)

// TestAlohaCollisionRateMatchesAnalytic calibrates the collision engine
// against the classic unslotted (pure) ALOHA result.
//
// Setup: N transmitters equidistant on a circle around one silent receiver,
// each sending exactly one frame of time-on-air T at an independent uniform
// random start time in [0, W). Equal distance means equal RSSI, so the 6 dB
// capture threshold can never be met and every time-overlap at the receiver
// is a collision: exactly the pure-ALOHA vulnerability model.
//
// Math: for two independent starts U, V ~ Uniform[0, W), the frames
// [U, U+T) and [V, V+T) overlap iff |U - V| < T, which has probability
//
//	p = 2T/W - (T/W)^2        (triangle distribution of |U - V|)
//
// A given frame survives iff none of the other N-1 frames overlap it:
//
//	P(success) = (1 - p)^(N-1)
//
// As N grows with offered load G = N*T/W held fixed, this converges to the
// textbook pure-ALOHA throughput factor e^(-2G): with the parameters below
// (N = 40, T = 263.168 ms, the default SF9/125 kHz cost of a 32-byte frame,
// W = 60 s), p = 0.008753 and P(success) = (1 - p)^39 = 0.7097, versus
// e^(-2G * (N-1)/N) = 0.7103.
//
// The test measures the delivered fraction over many seeded trials and
// requires it to match the exact finite-N expectation within 0.05
// (3.8 standard errors at 1200 samples).
func TestAlohaCollisionRateMatchesAnalytic(t *testing.T) {
	const (
		nTx     = 40
		windowS = 60.0
		runs    = 30
		radius  = 100.0
	)

	rng := rand.New(rand.NewSource(20260612))

	var delivered, total int
	var toaUs uint64

	for run := 0; run < runs; run++ {
		h := newRadioHarness()
		// LBT off: pure ALOHA assumes transmit-when-ready. This calibrates
		// the raw overlap engine; LBT behavior has its own unit tests.
		h.disableLBT()

		const rxAddr = uint32(0x10000)
		h.addNode(rxAddr, 0, 0)
		for i := 0; i < nTx; i++ {
			angle := 2 * math.Pi * float64(i) / nTx
			h.addNode(uint32(0x20000+i),
				float32(radius*math.Cos(angle)), float32(radius*math.Sin(angle)))
		}

		toaUs = uint64(h.frameAirtimeUs(frameBytes))
		windowUs := int64(windowS * 1e6)

		starts := make([]int64, nTx)
		for i := range starts {
			starts[i] = rng.Int63n(windowUs)
		}
		// Transmit in chronological order, evaluating due receptions before
		// each send, exactly as the real event loop does.
		order := make([]int, nTx)
		for i := range order {
			order[i] = i
		}
		sort.Slice(order, func(a, b int) bool { return starts[order[a]] < starts[order[b]] })

		var results []rxResult
		for _, node := range order {
			results = append(results, h.receptionsUntil(uint64(starts[node]))...)
			h.transmit(uint32(0x20000+node), rxAddr, frameBytes, uint64(starts[node]))
		}
		results = append(results, h.receptions()...)

		for _, r := range results {
			if r.destAddr != rxAddr {
				continue
			}
			total++
			switch r.outcome {
			case rxOutcomeOK:
				delivered++
			case rxOutcomeCaptured:
				t.Fatalf("capture at equal RSSI should be impossible")
			case rxOutcomeHalfDuplex:
				t.Fatalf("half-duplex drop at a silent receiver should be impossible")
			}
		}
		h.free()
	}

	if total != nTx*runs {
		t.Fatalf("expected %d receptions, got %d", nTx*runs, total)
	}

	tw := float64(toaUs) / (windowS * 1e6)
	pOverlap := 2*tw - tw*tw
	expected := math.Pow(1-pOverlap, nTx-1)
	measured := float64(delivered) / float64(total)

	t.Logf("ALOHA calibration: ToA=%v us, measured=%.4f, analytic=%.4f (G=%.3f)",
		toaUs, measured, expected, float64(nTx)*tw)
	if math.Abs(measured-expected) > 0.05 {
		t.Fatalf("measured success rate %.4f deviates from analytic %.4f by more than 0.05",
			measured, expected)
	}
}
