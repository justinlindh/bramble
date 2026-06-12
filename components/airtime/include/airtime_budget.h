#ifndef BRAMBLE_AIRTIME_BUDGET_H
#define BRAMBLE_AIRTIME_BUDGET_H
#include <stdint.h>
#include <stdbool.h>

#define AIRTIME_BUDGET_CRITICAL_MS 36000u
#define AIRTIME_BUDGET_NORMAL_MS 18000u
#define AIRTIME_BUDGET_BROADCAST_MS 18000u
#define AIRTIME_BUDGET_RECEIPT_MS 12000u
#define AIRTIME_REFILL_INTERVAL_MS 3600000u

/* Protocol tier codes (used in packet headers and RPC) */
#define AIRTIME_TIER_NORMAL 0x01
#define AIRTIME_TIER_CRITICAL 0x02
#define AIRTIME_TIER_BROADCAST 0x03
#define AIRTIME_TIER_RECEIPT 0x04

enum {
    AIRTIME_IDX_NORMAL = 0,
    AIRTIME_IDX_CRITICAL = 1,
    AIRTIME_IDX_BROADCAST = 2,
    AIRTIME_IDX_RECEIPT = 3,
    AIRTIME_TIER_COUNT = 4,
};

typedef struct {
    uint32_t tokens_ms[AIRTIME_TIER_COUNT];
    uint32_t max_ms[AIRTIME_TIER_COUNT];
    uint32_t base_max_ms[AIRTIME_TIER_COUNT];
    uint32_t refill_remainder[AIRTIME_TIER_COUNT];
    uint32_t last_refill_ms;
    uint8_t profile_peer_count;
    /* Regulatory duty-cycle cap (DES-8): when enforced, the sum of tier
     * maxima (and therefore the hourly refill) cannot exceed duty_cap_ms. */
    uint32_t duty_cap_ms;
    bool duty_enforced;
} airtime_budget_t;

void airtime_budget_init(airtime_budget_t* ab, uint32_t now_ms);
void airtime_budget_refill(airtime_budget_t* ab, uint32_t now_ms);
void airtime_budget_set_mesh_size(airtime_budget_t* ab, uint8_t peer_count);
/* Apply a regulatory duty-cycle limit (from freq_plan max_duty_cycle_pct /
 * duty_cycle_enforced). When enforced, every mesh-size profile is scaled
 * proportionally so the total TX budget stays within max_duty_cycle_pct of
 * the refill interval (e.g. EU868: 1% of an hour = 36000 ms). */
void airtime_budget_set_duty_cap(airtime_budget_t* ab, uint8_t max_duty_cycle_pct, bool enforced);
bool airtime_budget_can_transmit(airtime_budget_t* ab, uint8_t tier, uint32_t airtime_ms);
void airtime_budget_debit(airtime_budget_t* ab, uint8_t tier, uint32_t airtime_ms);
uint32_t airtime_budget_remaining(const airtime_budget_t* ab, uint8_t tier);
uint32_t airtime_budget_next_refill_ms(const airtime_budget_t* ab, uint32_t now_ms);
#endif
