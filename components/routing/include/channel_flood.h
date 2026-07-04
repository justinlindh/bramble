#ifndef BRAMBLE_CHANNEL_FLOOD_H
#define BRAMBLE_CHANNEL_FLOOD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool should_relay;
    uint8_t new_hop_limit;
    uint32_t jitter_ms;
} channel_flood_decision_t;

/*
 * The multi-hop channel/broadcast flood relay decision (Phase 1 delivery-
 * core Task 5). Broadcast/channel DATA (dest_addr == 0xFFFFFFFF) is
 * delivered locally today but never rebroadcast, so a group message only
 * ever reaches direct radio neighbors. This is the pure decision that
 * fixes that: whether THIS node, having just received/delivered a
 * broadcast DATA frame, should also relay it onward.
 *
 * This is a fresh design, not a revival of the caller-less channel_flood.c
 * deleted in 703d78a1: that version hardcoded its own flat 50-300ms jitter
 * constants and took a dedup_buffer_t pointer + packet_id directly (doing
 * its own packet_id-only dedup lookup inside the "pure" function, which
 * both made it untestable without a real dedup buffer and reused a
 * packet_id-only key -- the delivery-path audit's flagged cross-source
 * collision risk: two different originators' broadcasts could collide on
 * a 32-bit packet_id and one would be silently dropped as the other's
 * duplicate). This version:
 *   - takes the dup-check RESULT as a plain bool, so the caller decides how
 *     to key its dedup lookup (mesh_task.c uses a src_addr-qualified key
 *     for exactly this reason) and this function stays trivially testable;
 *   - reuses the RREQ forward path's jitter range (RREQ_FWD_JITTER_MIN_MS/
 *     RREQ_FWD_JITTER_MAX_MS, discovery.h) via discovery_forward_jitter_ms
 *     instead of a second hardcoded constant set, so same-hop relays
 *     already avoid keying up simultaneously for RREQ and now for channel
 *     floods too, with one tuning knob instead of two;
 *   - takes the airtime budget's real permit/deny as an explicit input
 *     (budget_permits), so a saturated node stops relaying instead of
 *     amplifying a storm -- the airtime-aware lever the plan calls for.
 *
 * Inputs:
 *   hop_limit      - the RECEIVED header hop_limit (before any decrement).
 *   is_duplicate   - true if the caller's dedup lookup already saw this
 *                     broadcast (src_addr + packet_id, or equivalent).
 *   budget_permits - true if the real airtime budget (BROADCAST lane; see
 *                     tx_gate_can_transmit/tx_gate_check) currently allows
 *                     transmitting a frame of this size. Non-mutating: the
 *                     caller's actual mesh_tx() still performs the real
 *                     check-and-debit at the jittered send time, so this is
 *                     a pre-check, not the final word.
 *   random_value   - caller-supplied randomness (e.g. esp_random()) for the
 *                     jitter draw. Kept as an explicit input, mirroring
 *                     discovery_forward_jitter_ms itself, so this function
 *                     stays pure and deterministic under test.
 *
 * Rules (first match wins):
 *   - hop_limit <= 1 (exhausted -- a relay receiving 1 does not forward,
 *     matching forward_data()/RREQ's ">1" convention, so hop_limit N means
 *     exactly N-hop reach) -> no relay.
 *   - is_duplicate -> no relay (already flooded by/through this node).
 *   - !budget_permits -> no relay (airtime-aware: a saturated node yields).
 *   - otherwise -> relay, with new_hop_limit = hop_limit - 1 and jitter_ms
 *     drawn from the shared RREQ_FWD_JITTER_MIN_MS..RREQ_FWD_JITTER_MAX_MS
 *     range.
 */
channel_flood_decision_t channel_flood_decide(uint8_t hop_limit, bool is_duplicate,
                                              bool budget_permits, uint32_t random_value);

#endif
