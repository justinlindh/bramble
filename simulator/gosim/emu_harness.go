package main

/*
#include "bridge.h"
*/
import "C"
import (
	"strings"
	"sync"
)

// emuHarness drives the emu-link broker against a live Sim for the extnode
// tests. Like radio_harness.go, it exists so _test.go files (which cannot
// import "C") can exercise the broker, the radio model, and the real-time
// event pump through a pure-Go API. It captures every UI-side JSON event the
// Sim broadcasts and starts the C-stdout pipe reader so the C emitter never
// blocks on a full pipe while a transmission is being priced.
type emuHarness struct {
	sim *Sim

	mu    sync.Mutex
	lines []string
}

// newEmuHarness builds a Sim with the default (SF10/125 kHz) radio config and a
// running pipe reader, ready for a broker to attach. Call close() to restore
// stdout when done.
func newEmuHarness() *emuHarness {
	h := &emuHarness{}
	sim, err := NewSim("", func(b []byte) {
		h.mu.Lock()
		h.lines = append(h.lines, strings.TrimRight(string(b), "\n"))
		h.mu.Unlock()
	}, true)
	if err != nil {
		panic(err)
	}
	radioConfigInit(&sim.radio)
	pcg32Seed(&sim.rng, 42)
	go sim.readPipe()
	h.sim = sim
	return h
}

// startBroker opens the emu-link socket at path and attaches it to the Sim.
func (h *emuHarness) startBroker(path string) error {
	b, err := NewBroker(h.sim, path)
	if err != nil {
		return err
	}
	h.sim.broker = b
	b.Start()
	return nil
}

// reserveSlot reserves an external-node position (attach order binds slots).
func (h *emuHarness) reserveSlot(x, y float32, label string) {
	h.sim.broker.reserveSlot(x, y, label, "")
}

// setLBT toggles listen-before-talk on the shared radio config (off lets
// overlapping transmissions actually collide instead of being backed off).
func (h *emuHarness) setLBT(on bool) { h.sim.radio.lbt_enabled = C.bool(on) }

// setCollisions toggles the collision/half-duplex model.
func (h *emuHarness) setCollisions(on bool) { h.sim.radio.collisions_enabled = C.bool(on) }

// advance pushes the simulation clock forward by deltaUs and processes every
// event and broker action that has come due, exactly as the real-time loop
// does one tick at a time.
func (h *emuHarness) advance(deltaUs uint64) {
	h.sim.mu.Lock()
	h.sim.pump(h.sim.simTime + deltaUs)
	h.sim.mu.Unlock()
}

// toaMs returns the deterministic time-on-air (ms) the broker prices an
// n-byte frame at, for cross-checking a txdone against the radio model.
func (h *emuHarness) toaMs(n int) uint32 {
	return uint32(C.radio_frame_airtime_ms(&h.sim.radio, C.uint16_t(n)))
}

// close tears the broker down and restores the process stdout.
func (h *emuHarness) close() {
	if h.sim.broker != nil {
		h.sim.broker.Stop()
	}
	h.sim.pipeW.Close()
	dup2Stdout(h.sim.origStdout)
	closeFd(h.sim.origStdout)
}
