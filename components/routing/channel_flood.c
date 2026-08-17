#include "channel_flood.h"
#include "discovery.h"
#include "routing.h" /* ROUTE_HOP_LIMIT_MAX (reactive origination budget) */

#include <string.h>

channel_flood_decision_t channel_flood_decide(uint8_t hop_limit, bool is_duplicate,
                                              bool budget_permits, uint32_t random_value) {
    channel_flood_decision_t d = {false, 0, 0};

    if (hop_limit <= 1 || is_duplicate || !budget_permits) {
        return d;
    }

    d.should_relay = true;
    d.new_hop_limit = hop_limit - 1;
    /* Reuse the DES-3 RREQ forward jitter range (discovery.h) rather than a
     * second hardcoded constant set; see channel_flood.h for the rationale. */
    d.jitter_ms = discovery_forward_jitter_ms(random_value);
    return d;
}

uint8_t flood_hop_limit_clamp(uint32_t hops) {
    if (hops < FLOOD_HOP_LIMIT_MIN) {
        return FLOOD_HOP_LIMIT_MIN;
    }
    if (hops > FLOOD_HOP_LIMIT_CEIL) {
        return FLOOD_HOP_LIMIT_CEIL;
    }
    return (uint8_t)hops;
}

uint8_t flood_origination_hop_limit(bool flood_transport, uint32_t flood_hop_limit) {
    /* Flood transport: originate at the clamped operator-settable budget.
     * Reactive (default): originate at ROUTE_HOP_LIMIT_MAX, unchanged. */
    return flood_transport ? flood_hop_limit_clamp(flood_hop_limit) : ROUTE_HOP_LIMIT_MAX;
}

bool channel_flood_note_overheard(pending_flood_relay_t* queue, int capacity, uint32_t flood_key) {
    for (int i = 0; i < capacity; i++) {
        if (!queue[i].used || queue[i].flood_key != flood_key) {
            continue;
        }
        /* Src-qualified match: this overheard copy is of the SAME frame
         * (packet_id ^ src_addr) this pending relay would rebroadcast. Count
         * it, and once enough OTHER copies have been overheard, cancel our
         * own now-redundant relay so process_flood_relay_queue never fires
         * it (used=false). */
        queue[i].heard++;
        if (queue[i].heard >= FLOOD_SUPPRESS_AFTER) {
            queue[i].used = false;
            return true;
        }
        return false;
    }
    return false;
}

bool channel_flood_relay_admit(pending_flood_relay_t* queue, int capacity, const uint8_t* buf,
                               uint8_t len, uint32_t due_at_ms, uint32_t flood_key, uint8_t tx_kind,
                               uint32_t* drop_counter) {
    /* No length guard needed: len is uint8_t and pending_flood_relay_t::buf
     * is BRAMBLE_MAX_PACKET_SIZE (256), so the memcpy below cannot overrun.
     * Static assert so a future shrink of buf turns into a build error
     * rather than a silent stack smash. */
    _Static_assert(BRAMBLE_MAX_PACKET_SIZE >= 256,
                   "pending_flood_relay_t::buf must hold any uint8_t length");

    for (int i = 0; i < capacity; i++) {
        if (queue[i].used) {
            continue;
        }
        queue[i].used = true;
        queue[i].due_at_ms = due_at_ms;
        memcpy(queue[i].buf, buf, len);
        queue[i].len = len;
        queue[i].flood_key = flood_key;
        queue[i].heard = 0;
        queue[i].tx_kind = tx_kind;
        return true;
    }

    /* Queue full: DROP. There is deliberately no immediate-transmit branch
     * here; see channel_flood.h for why adding one inverts backpressure. */
    if (drop_counter)
        (*drop_counter)++;
    return false;
}
