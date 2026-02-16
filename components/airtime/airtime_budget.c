#include "airtime_budget.h"

void airtime_budget_init(airtime_budget_t *ab, uint32_t now_ms) {
    ab->max_ms[0] = AIRTIME_BUDGET_BROADCAST_MS;
    ab->max_ms[1] = AIRTIME_BUDGET_NORMAL_MS;
    ab->max_ms[2] = AIRTIME_BUDGET_CRITICAL_MS;
    for (int i = 0; i < 3; i++) {
        ab->tokens_ms[i] = ab->max_ms[i];
    }
    ab->last_refill_ms = now_ms;
}

void airtime_budget_refill(airtime_budget_t *ab, uint32_t now_ms) {
    if (now_ms - ab->last_refill_ms >= AIRTIME_REFILL_INTERVAL_MS) {
        for (int i = 0; i < 3; i++) {
            ab->tokens_ms[i] = ab->max_ms[i];
        }
        ab->last_refill_ms = now_ms;
    }
}

bool airtime_budget_can_transmit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms) {
    if (tier > 2) return false;
    if (ab->tokens_ms[tier] >= airtime_ms) return true;
    // Critical (tier 2) can borrow from normal (tier 1)
    if (tier == 2) {
        uint32_t deficit = airtime_ms - ab->tokens_ms[2];
        if (ab->tokens_ms[1] >= deficit) return true;
    }
    return false;
}

void airtime_budget_debit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms) {
    if (tier > 2) return;
    if (ab->tokens_ms[tier] >= airtime_ms) {
        ab->tokens_ms[tier] -= airtime_ms;
    } else if (tier == 2) {
        uint32_t deficit = airtime_ms - ab->tokens_ms[2];
        ab->tokens_ms[2] = 0;
        if (ab->tokens_ms[1] >= deficit) {
            ab->tokens_ms[1] -= deficit;
        } else {
            ab->tokens_ms[1] = 0;
        }
    } else {
        ab->tokens_ms[tier] = 0;
    }
}

uint32_t airtime_budget_remaining(const airtime_budget_t *ab, uint8_t tier) {
    if (tier > 2) return 0;
    return ab->tokens_ms[tier];
}
