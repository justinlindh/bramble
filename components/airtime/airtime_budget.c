#include "airtime_budget.h"

/*
 * Tier mapping: protocol tier codes -> array indices
 *   0x01 (NORMAL)    -> index 0
 *   0x02 (CRITICAL)  -> index 1
 *   0x03 (BROADCAST) -> index 2
 *   0x04 (RECEIPT)   -> index 3
 */
static inline int tier_idx(uint8_t tier) {
    if (tier == AIRTIME_TIER_CRITICAL) return AIRTIME_IDX_CRITICAL;
    if (tier == AIRTIME_TIER_BROADCAST) return AIRTIME_IDX_BROADCAST;
    if (tier == AIRTIME_TIER_RECEIPT) return AIRTIME_IDX_RECEIPT;
    return AIRTIME_IDX_NORMAL;
}

/* Adaptive profile by peer count:
 *   <=8:  micro mesh (very relaxed — dev clusters, small deployments)
 *   9..15: small mesh (relaxed)
 *   16..40: baseline
 *   >40: large mesh (conservative broadcast/receipt)
 */
static uint32_t profile_scale_pct(uint8_t peer_count, int idx) {
    if (peer_count <= 8u) {
        /* Micro mesh: aggressive budgets for reliable delivery in small clusters.
         * At 5 nodes, collision is the primary delivery failure mode — give
         * enough airtime for retries to succeed. */
        if (idx == AIRTIME_IDX_BROADCAST) return 400u;
        if (idx == AIRTIME_IDX_RECEIPT) return 500u;
        if (idx == AIRTIME_IDX_NORMAL) return 300u;
        return 100u; /* critical */
    }
    if (peer_count <= 15u) {
        if (idx == AIRTIME_IDX_BROADCAST || idx == AIRTIME_IDX_RECEIPT) return 250u;
        if (idx == AIRTIME_IDX_NORMAL) return 150u;
        return 100u; /* critical */
    }
    if (peer_count > 40u) {
        if (idx == AIRTIME_IDX_BROADCAST) return 60u;
        if (idx == AIRTIME_IDX_RECEIPT) return 50u;
        if (idx == AIRTIME_IDX_NORMAL) return 75u;
        return 100u; /* critical */
    }
    return 100u; /* baseline */
}

static void apply_profile(airtime_budget_t *ab, uint8_t peer_count) {
    ab->profile_peer_count = peer_count;
    for (int i = 0; i < AIRTIME_TIER_COUNT; i++) {
        uint32_t scaled = (ab->base_max_ms[i] * profile_scale_pct(peer_count, i)) / 100u;
        if (scaled == 0u) scaled = 1u;
        ab->max_ms[i] = scaled;
        if (ab->tokens_ms[i] > ab->max_ms[i]) {
            ab->tokens_ms[i] = ab->max_ms[i];
        } else if (ab->tokens_ms[i] < ab->max_ms[i]) {
            /* Keep profile transitions simple/predictable: tokens follow new cap. */
            ab->tokens_ms[i] = ab->max_ms[i];
        }
        /* reset fractional carry when max changes to avoid stale drift */
        ab->refill_remainder[i] = 0u;
    }
}

void airtime_budget_init(airtime_budget_t *ab, uint32_t now_ms) {
    ab->base_max_ms[AIRTIME_IDX_NORMAL] = AIRTIME_BUDGET_NORMAL_MS;
    ab->base_max_ms[AIRTIME_IDX_CRITICAL] = AIRTIME_BUDGET_CRITICAL_MS;
    ab->base_max_ms[AIRTIME_IDX_BROADCAST] = AIRTIME_BUDGET_BROADCAST_MS;
    ab->base_max_ms[AIRTIME_IDX_RECEIPT] = AIRTIME_BUDGET_RECEIPT_MS;

    for (int i = 0; i < AIRTIME_TIER_COUNT; i++) {
        ab->tokens_ms[i] = ab->base_max_ms[i];
        ab->max_ms[i] = ab->base_max_ms[i];
        ab->refill_remainder[i] = 0u;
    }
    ab->profile_peer_count = 0u;
    ab->last_refill_ms = now_ms;
    apply_profile(ab, 0u);
}

void airtime_budget_set_mesh_size(airtime_budget_t *ab, uint8_t peer_count) {
    if (ab->profile_peer_count == peer_count) return;
    apply_profile(ab, peer_count);
}

void airtime_budget_refill(airtime_budget_t *ab, uint32_t now_ms) {
    uint32_t elapsed = now_ms - ab->last_refill_ms;
    if (elapsed == 0u) return;

    for (int i = 0; i < AIRTIME_TIER_COUNT; i++) {
        uint64_t numer = (uint64_t)ab->refill_remainder[i] + ((uint64_t)ab->max_ms[i] * (uint64_t)elapsed);
        uint32_t add = (uint32_t)(numer / AIRTIME_REFILL_INTERVAL_MS);
        ab->refill_remainder[i] = (uint32_t)(numer % AIRTIME_REFILL_INTERVAL_MS);

        if (add > 0u) {
            uint64_t next = (uint64_t)ab->tokens_ms[i] + (uint64_t)add;
            ab->tokens_ms[i] = (next >= ab->max_ms[i]) ? ab->max_ms[i] : (uint32_t)next;
        }
    }

    ab->last_refill_ms = now_ms;
}

bool airtime_budget_can_transmit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms) {
    int idx = tier_idx(tier);
    if (ab->tokens_ms[idx] >= airtime_ms) return true;
    /* Critical can borrow from normal */
    if (idx == AIRTIME_IDX_CRITICAL) {
        uint32_t deficit = airtime_ms - ab->tokens_ms[AIRTIME_IDX_CRITICAL];
        if (ab->tokens_ms[AIRTIME_IDX_NORMAL] >= deficit) return true;
    }
    return false;
}

void airtime_budget_debit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms) {
    int idx = tier_idx(tier);
    if (ab->tokens_ms[idx] >= airtime_ms) {
        ab->tokens_ms[idx] -= airtime_ms;
    } else if (idx == AIRTIME_IDX_CRITICAL) {
        /* Critical borrows from normal */
        uint32_t deficit = airtime_ms - ab->tokens_ms[AIRTIME_IDX_CRITICAL];
        ab->tokens_ms[AIRTIME_IDX_CRITICAL] = 0u;
        if (ab->tokens_ms[AIRTIME_IDX_NORMAL] >= deficit) {
            ab->tokens_ms[AIRTIME_IDX_NORMAL] -= deficit;
        } else {
            ab->tokens_ms[AIRTIME_IDX_NORMAL] = 0u;
        }
    } else {
        ab->tokens_ms[idx] = 0u;
    }
}

uint32_t airtime_budget_remaining(const airtime_budget_t *ab, uint8_t tier) {
    return ab->tokens_ms[tier_idx(tier)];
}

uint32_t airtime_budget_next_refill_ms(const airtime_budget_t *ab, uint32_t now_ms) {
    (void)ab;
    (void)now_ms;
    /* Continuous refill model: tokens start accruing immediately. */
    return 0u;
}
