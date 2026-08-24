package main

import "testing"

// Route-poisoning demonstration (simulation).
//
// An unauthenticated RREQ carries an attacker-controlled prev_hop, hop_count
// and metric. Firmware's handle_rreq, after answering an RREQ addressed to it,
// installs a route back toward that source: dest = next_hop = prev_hop, with
// the RREQ's hop_count/metric. If that route were classed ROUTE_SRC_DISCOVERED,
// the most-trusted class, its route_install invariants refuse to let anything
// displace or evict it, so a keyless attacker could flood an RREQ (prev_hop =
// a node it wants to impersonate, hop_count forged to 0, metric forged to the
// maximal 255) and install an unbeatable, permanent poisoned route.
//
// These two tests drive the SAME real firmware routing code through the gosim
// cgo bridge (bridge.c's _handle_rreq -> routing.c's route_install), toggling
// only the trust class the source-route install uses, to show the attack
// succeeding under a ROUTE_SRC_DISCOVERED classing and being neutralized
// under the shipped ROUTE_SRC_BREADCRUMB classing. This is a simulation; it is
// not hardware verification.

const (
	poisonVictimAddr = 0x000000E0 // the node under attack
	poisonTargetAddr = 0x000000E1 // address the attacker impersonates via prev_hop
	poisonLegitHop   = 0x000000E2 // the real next hop of the victim's honest route
)

// seedAndFlood installs a legitimate multi-hop DISCOVERED route to the target
// on the victim, then floods forged RREQs (addressed to the victim, so it
// takes the "RREQ is for us" branch and installs the source route) under the
// given trust class. It returns the victim's resulting route to the target.
func seedAndFlood(t *testing.T, trustClass int) (nextHop uint32, source int, found bool) {
	t.Helper()
	h := newRadioHarness()
	defer h.free()
	h.provisionAll()

	h.addNode(poisonVictimAddr, 0, 0)
	// The forged RREQ's radio sender: an adjacent attacker spoofing the target
	// address at the link layer (prev_hop == its own claimed address). It must
	// exist as a node so the radio reception model can deliver its frame; the
	// spoofing is the whole point of the attack.
	h.addNode(poisonTargetAddr, 5, 0)
	victim := h.activateNode(poisonVictimAddr)
	h.activateNode(poisonTargetAddr)
	h.disableCollisions()

	// The victim's honest, control-plane route to the target: 3 hops away via
	// a real neighbour, a middling metric. This is what a legitimate signed
	// RREP would have installed.
	h.installDiscoveredRoute(victim, poisonTargetAddr, poisonLegitHop, 3, 100, 0)
	if nh, _, ok := h.routeEntry(victim, poisonTargetAddr); !ok || nh != poisonLegitHop {
		t.Fatalf("precondition failed: victim's honest route to target not seeded (found=%v next_hop=0x%08X)", ok, nh)
	}

	h.setRREQSrcRouteTrust(trustClass)
	defer h.clearRREQSrcRouteTrust()

	// Flood a handful of forged RREQs. Each is addressed to the victim
	// (rreqDest = victim) so the victim answers and installs a source route to
	// prev_hop = the target address. rreq_build_originator stamps hop_count 0
	// and metric 255: the strongest possible forged route.
	now := uint64(0)
	for i := 0; i < 5; i++ {
		h.deliverRREQ(victim, poisonTargetAddr, poisonVictimAddr, uint32(0x9000+i), 4, now)
		now += 50_000
	}

	return h.routeEntry(victim, poisonTargetAddr)
}

// TestRREQForgedRoutePoisonsUnderDiscoveredClassing demonstrates the
// vulnerability described above: with the source route classed
// ROUTE_SRC_DISCOVERED, the forged RREQ overwrites the victim's honest route
// to the target and the next hop is hijacked to the attacker-impersonated
// address.
func TestRREQForgedRoutePoisonsUnderDiscoveredClassing(t *testing.T) {
	nextHop, source, found := seedAndFlood(t, routeSourceDiscovered)

	if !found {
		t.Fatalf("expected a route to the target to exist after the flood")
	}
	if nextHop != poisonTargetAddr {
		t.Fatalf("expected the honest route to be hijacked to next_hop=0x%08X, got 0x%08X: the pre-fix DISCOVERED classing should let the forged RREQ win",
			poisonTargetAddr, nextHop)
	}
	if source != routeSourceDiscovered {
		t.Fatalf("expected poisoned route source = DISCOVERED (%d), got %d", routeSourceDiscovered, source)
	}
}

// TestRREQForgedRouteCannotPoisonUnderBreadcrumbClassing pins the guarantee:
// with the source route classed ROUTE_SRC_BREADCRUMB, the same forged RREQ
// flood cannot touch the victim's honest DISCOVERED route to the target,
// which survives intact via its real next hop.
func TestRREQForgedRouteCannotPoisonUnderBreadcrumbClassing(t *testing.T) {
	nextHop, source, found := seedAndFlood(t, routeSourceBreadcrumb)

	if !found {
		t.Fatalf("expected the victim's honest route to the target to still exist")
	}
	if nextHop != poisonLegitHop {
		t.Fatalf("honest route was corrupted: next_hop=0x%08X, want the real hop 0x%08X: a BREADCRUMB-classed RREQ source route must not displace a DISCOVERED route",
			nextHop, poisonLegitHop)
	}
	if source != routeSourceDiscovered {
		t.Fatalf("honest route trust class changed to %d, want DISCOVERED (%d)", source, routeSourceDiscovered)
	}
}
