package main

import "testing"

// TestRREQForwardRateLimiterBucketExhausts pins the middle node of a 3-node
// line (A -> B -> C): every forwarded RREQ runs through the real
// rreq_fwd_allow global token bucket (components/security), the same
// component and same decision point (after dedup, before the jittered
// rebroadcast) firmware's handle_rreq uses. The bucket is BURST=16 tokens
// refilling one token per 2s (RREQ_FWD_BURST / RREQ_FWD_REFILL_MS), so
// flooding more than 16 distinct RREQs at B within a couple hundred ms must
// stop forwarding at 16 and record the rest as denied.
func TestRREQForwardRateLimiterBucketExhausts(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addrA = 0x000000A0 // originator / previous hop for every RREQ
	const addrB = 0x000000B0 // middle node under test
	const addrC = 0x000000C0 // RREQ's final destination (never reached here)
	h.addNode(addrA, 0, 0)
	h.addNode(addrB, 10, 0)
	h.addNode(addrC, 20, 0)

	b := h.activateNode(addrB)
	h.activateNode(addrA)
	h.activateNode(addrC)

	// Isolate the rate-limiter layer from the radio/half-duplex model: B's
	// own jittered forward rebroadcasts otherwise leave future occupancy
	// windows in the channel log that a later synthetic deliverRREQ can land
	// inside of, which is a real half-duplex effect but not what this test
	// is exercising (that model has its own tests in collision_test.go).
	h.disableCollisions()

	const distinctRREQs = 25
	const hopLimit = 4 // > 1, so B takes the forward branch rather than "for us"

	now := uint64(0)
	for i := 0; i < distinctRREQs; i++ {
		// Distinct query_id per RREQ (also becomes header.packet_id): each
		// one is a different discovery, not a dedup-collapsed retransmission.
		queryID := uint32(0x1000 + i)
		h.deliverRREQ(b, addrA, addrC, queryID, hopLimit, now)
		now += 50_000 // 50ms apart: 25 RREQs land within 1.25s, well inside
		// one 2s refill window, so no more than 1 extra token accrues.
	}

	forwarded := h.packetsForwarded(b)
	denied := h.rreqFwdDenied(b)

	if forwarded == 0 {
		t.Fatalf("expected some RREQs to be forwarded before the bucket exhausted, got 0")
	}
	if forwarded > 17 {
		t.Fatalf("forwarded = %d, want <= 17 (burst=16 plus at most one 2s-window refill)",
			forwarded)
	}
	if denied == 0 {
		t.Fatalf("rreq_fwd_denied = 0, want > 0: the burst bucket should have stopped forwarding")
	}
	if forwarded+uint64(denied) != distinctRREQs {
		t.Fatalf("forwarded (%d) + denied (%d) = %d, want %d (every RREQ is either forwarded or denied, none silently lost)",
			forwarded, denied, forwarded+uint64(denied), distinctRREQs)
	}
}

// TestRREQForwardRateLimiterAllowsUnderBurst is the control: fewer forwards
// than the burst size, spread out, should all succeed with zero denials.
func TestRREQForwardRateLimiterAllowsUnderBurst(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addrA = 0x000000A1
	const addrB = 0x000000B1
	const addrC = 0x000000C1
	h.addNode(addrA, 0, 0)
	h.addNode(addrB, 10, 0)
	h.addNode(addrC, 20, 0)

	b := h.activateNode(addrB)
	h.activateNode(addrA)
	h.activateNode(addrC)
	h.disableCollisions()

	now := uint64(0)
	for i := 0; i < 10; i++ {
		h.deliverRREQ(b, addrA, addrC, uint32(0x2000+i), 4, now)
		now += 50_000
	}

	if forwarded := h.packetsForwarded(b); forwarded != 10 {
		t.Fatalf("forwarded = %d, want 10 (under the burst of 16)", forwarded)
	}
	if denied := h.rreqFwdDenied(b); denied != 0 {
		t.Fatalf("rreq_fwd_denied = %d, want 0 under the burst", denied)
	}
}

// TestRREQOriginationRateLimiterDeniesWithinWindow pins
// bridge_handle_generate_message's fresh-discovery origination (the "!pd"
// branch, the sim's equivalent of firmware's initiate_discovery,
// main/mesh_task.c:4320): it consults the real rreq_rate_allow
// per-(self,dest) limiter (30s window) before starting a discovery, instead
// of originating unconditionally.
//
// Discovery retries (the "else if discovery_should_retry" branch, and
// node_tick's separate discovery retry loop) are deliberately NOT gated
// here: firmware only rate-limits the fresh-origination call, never the
// periodic retry loop (mesh_task.c:3129-3177), which self-throttles via its
// own backoff cadence instead.
func TestRREQOriginationRateLimiterDeniesWithinWindow(t *testing.T) {
	h := newRadioHarness()
	defer h.free()

	const addrA = 0x000000D0
	const addrDest = 0x000000D1
	h.addNode(addrA, 0, 0)
	a := h.activateNode(addrA)

	// First-ever origination for this dest: nothing in the limiter yet, must
	// go through and actually transmit.
	h.generateMessage(a, addrDest, 0)
	if denied := h.rreqRateDenied(a); denied != 0 {
		t.Fatalf("rreq_rate_denied = %d after the first-ever origination, want 0", denied)
	}
	sentAfterFirst := h.packetsSent(a)
	if sentAfterFirst == 0 {
		t.Fatalf("expected the first origination to actually transmit an RREQ")
	}

	// Simulate the discovery having been abandoned (exhausted retries) while
	// the rate-limiter's 30s window from the first attempt is still open,
	// then retry: this is exactly the case firmware's rreq_rate_allow exists
	// to catch (a node hammering the same destination with fresh discovery
	// attempts), so the second origination must be denied and must not
	// transmit.
	h.removePendingDiscovery(a, addrDest)
	h.generateMessage(a, addrDest, 1_000) // 1s later, well inside the 30s window

	if denied := h.rreqRateDenied(a); denied != 1 {
		t.Fatalf("rreq_rate_denied = %d, want 1 after a same-window re-origination", denied)
	}
	if sent := h.packetsSent(a); sent != sentAfterFirst {
		t.Fatalf("packets_sent changed %d -> %d on a rate-limited attempt, want no new transmission",
			sentAfterFirst, sent)
	}
}
