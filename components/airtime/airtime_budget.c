#include "airtime_budget.h"

/*
 * Tier mapping: protocol tier codes → array indices
 *   0x01 (NORMAL)    → index 0
 *   0x02 (CRITICAL)  → index 1
 *   0x03 (BROADCAST) → index 2
 *
 * Index 0 is also the default for unknown tiers.
 */
static inline int tier_idx(uint8_t tier) {
    if (tier == AIRTIME_TIER_CRITICAL)  return 1;
    if (tier == AIRTIME_TIER_BROADCAST) return 2;
    return 0; /* NORMAL or unknown */
}

void airtime_budget_init(airtime_budget_t *ab, uint32_t now_ms) {
    ab->max_ms[0] = AIRTIME_BUDGET_NORMAL_MS;
    ab->max_ms[1] = AIRTIME_BUDGET_CRITICAL_MS;
    ab->max_ms[2] = AIRTIME_BUDGET_BROADCAST_MS;
    for (int i = 0; i < 3; i++) {
        ab->tokens_ms[i] = ab->max_ms[i];
    }
    ab->last_refill_ms = now_ms;
}

void airtime_budget_refill(airtime_budget_t *ab, uint32_t now_ms) {
    uint32_t elapsed = now_ms - ab->last_refill_ms;
    if (elapsed >= AIRTIME_REFILL_INTERVAL_MS) {
        /* Full refill */
        for (int i = 0; i < 3; i++) {
            ab->tokens_ms[i] = ab->max_ms[i];
        }
        ab->last_refill_ms = now_ms;
    }
}

bool airtime_budget_can_transmit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms) {
    int idx = tier_idx(tier);
    if (ab->tokens_ms[idx] >= airtime_ms) return true;
    /* Critical can borrow from normal */
    if (idx == 1) {
        uint32_t deficit = airtime_ms - ab->tokens_ms[1];
        if (ab->tokens_ms[0] >= deficit) return true;
    }
    return false;
}

void airtime_budget_debit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms) {
    int idx = tier_idx(tier);
    if (ab->tokens_ms[idx] >= airtime_ms) {
        ab->tokens_ms[idx] -= airtime_ms;
    } else if (idx == 1) {
        /* Critical borrows from normal */
        uint32_t deficit = airtime_ms - ab->tokens_ms[1];
        ab->tokens_ms[1] = 0;
        if (ab->tokens_ms[0] >= deficit) {
            ab->tokens_ms[0] -= deficit;
        } else {
            ab->tokens_ms[0] = 0;
        }
    } else {
        ab->tokens_ms[idx] = 0;
    }
}

uint32_t airtime_budget_remaining(const airtime_budget_t *ab, uint8_t tier) {
    return ab->tokens_ms[tier_idx(tier)];
}

uint32_t airtime_budget_next_refill_ms(const airtime_budget_t *ab, uint32_t now_ms) {
    uint32_t elapsed = now_ms - ab->last_refill_ms;
    if (elapsed >= AIRTIME_REFILL_INTERVAL_MS) return 0;
    return AIRTIME_REFILL_INTERVAL_MS - elapsed;
}
