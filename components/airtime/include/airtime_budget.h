#ifndef BRAMBLE_AIRTIME_BUDGET_H
#define BRAMBLE_AIRTIME_BUDGET_H
#include <stdint.h>
#include <stdbool.h>

#define AIRTIME_BUDGET_CRITICAL_MS   36000
#define AIRTIME_BUDGET_NORMAL_MS     18000
#define AIRTIME_BUDGET_BROADCAST_MS  18000
#define AIRTIME_REFILL_INTERVAL_MS   3600000

/* Protocol tier codes (used in packet headers and RPC) */
#define AIRTIME_TIER_NORMAL    0x01
#define AIRTIME_TIER_CRITICAL  0x02
#define AIRTIME_TIER_BROADCAST 0x03

typedef struct {
    uint32_t tokens_ms[3];
    uint32_t max_ms[3];
    uint32_t last_refill_ms;
} airtime_budget_t;

void airtime_budget_init(airtime_budget_t *ab, uint32_t now_ms);
void airtime_budget_refill(airtime_budget_t *ab, uint32_t now_ms);
bool airtime_budget_can_transmit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms);
void airtime_budget_debit(airtime_budget_t *ab, uint8_t tier, uint32_t airtime_ms);
uint32_t airtime_budget_remaining(const airtime_budget_t *ab, uint8_t tier);
uint32_t airtime_budget_next_refill_ms(const airtime_budget_t *ab, uint32_t now_ms);
#endif
