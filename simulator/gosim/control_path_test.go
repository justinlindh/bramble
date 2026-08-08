package main

import (
	"path/filepath"
	"testing"
	"time"
)

// The emulator control path (emulator/node/emu_control.c) is what lets a person
// drive an inert fleet from the browser: "prov" provisions the network key at
// runtime, "send" originates a message. These tests assert the broker half of
// that contract, since the firmware half is exercised by the playground
// scenario in emulator/ci/run_scenarios.sh.

// A fleet-wide provision reaches every attached node, carrying the key
// verbatim. This is the playground's one-click "provision the fleet".
func TestProvisionFleetReachesEveryAttachedNode(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	h.reserveSlot(100, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	key := "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
	h.sim.handleCommand(Command{Type: "prov", Key: key})

	for name, fn := range map[string]*fakeNode{"A": nodeA, "B": nodeB} {
		m := fn.expect(t, "prov", 2*time.Second)
		if m == nil {
			t.Fatalf("node %s never received the provision message", name)
		}
		if m["key"] != key {
			t.Fatalf("node %s got key %v, want %v", name, m["key"], key)
		}
	}
}

// Naming a node provisions that node ALONE. The playground's teaching moment
// (a still-inert neighbor beside a provisioned one) depends on this: a
// fleet-wide default that quietly keyed everyone would erase it.
func TestProvisionSingleNodeLeavesTheRestInert(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	h.reserveSlot(100, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	key := "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
	h.sim.handleCommand(Command{Type: "prov", Node: "A", Key: key})

	if m := nodeA.expect(t, "prov", 2*time.Second); m == nil {
		t.Fatal("node A never received the provision message")
	}
	if m := nodeB.read(300 * time.Millisecond); m != nil {
		t.Fatalf("node B was provisioned too: %v", m)
	}
}

// A malformed key never reaches the firmware. The node would drop it anyway,
// silently; refusing at the broker keeps the failure visible in one place.
func TestProvisionRejectsMalformedKey(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()

	for _, bad := range []string{"", "0102", "zz02030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"} {
		h.sim.handleCommand(Command{Type: "prov", Key: bad})
		if m := nodeA.read(200 * time.Millisecond); m != nil {
			t.Fatalf("malformed key %q reached the node: %v", bad, m)
		}
	}
}

// A send names its sender and carries the destination: empty "to" is a channel
// broadcast, an 8-hex address is a DM.
func TestSendRoutesToTheNamedSender(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	h.reserveSlot(100, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	h.sim.handleCommand(Command{Type: "send", Node: "A", Text: "HELLO PLAYGROUND"})
	m := nodeA.expect(t, "send", 2*time.Second)
	if m == nil {
		t.Fatal("node A never received the send message")
	}
	if m["text"] != "HELLO PLAYGROUND" {
		t.Fatalf("text = %v, want HELLO PLAYGROUND", m["text"])
	}
	if to, ok := m["to"]; ok && to != "" {
		t.Fatalf("broadcast carried a destination: %v", to)
	}
	if m := nodeB.read(300 * time.Millisecond); m != nil {
		t.Fatalf("node B received a send addressed to A: %v", m)
	}

	h.sim.handleCommand(Command{Type: "send", Node: "B", Text: "DM TEXT", To: "1A2B3C4D"})
	m = nodeB.expect(t, "send", 2*time.Second)
	if m == nil {
		t.Fatal("node B never received the DM send message")
	}
	if m["to"] != "1A2B3C4D" {
		t.Fatalf("to = %v, want 1A2B3C4D", m["to"])
	}
}

// An identity announcement goes to the one node that was asked for it. The
// playground's safety-number step depends on this: a peer can only be verified
// once its identity is pinned, and a pin only comes from an attestation.
func TestAttestReachesOnlyTheNamedNode(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	h.reserveSlot(100, 0, "B")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()
	nodeB := dialFakeNode(t, path, "B")
	defer nodeB.close()

	h.sim.handleCommand(Command{Type: "attest", Node: "A"})
	if m := nodeA.expect(t, "attest", 2*time.Second); m == nil {
		t.Fatal("node A never received the attest message")
	}
	if m := nodeB.read(300 * time.Millisecond); m != nil {
		t.Fatalf("node B was told to attest too: %v", m)
	}

	// An unnamed node is refused rather than fanned out to the fleet: every
	// node announcing in the same instant is the collision the real cadence
	// exists to spread out.
	h.sim.handleCommand(Command{Type: "attest"})
	if m := nodeA.read(300 * time.Millisecond); m != nil {
		t.Fatalf("an unnamed attest reached a node: %v", m)
	}
}

// An empty text or a non-address destination is refused at the broker.
func TestSendRejectsEmptyTextAndBadDestination(t *testing.T) {
	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	h.reserveSlot(0, 0, "A")
	nodeA := dialFakeNode(t, path, "A")
	defer nodeA.close()

	h.sim.handleCommand(Command{Type: "send", Node: "A", Text: ""})
	h.sim.handleCommand(Command{Type: "send", Node: "A", Text: "x", To: "not-an-addr"})
	if m := nodeA.read(300 * time.Millisecond); m != nil {
		t.Fatalf("a rejected send reached the node: %v", m)
	}
}
