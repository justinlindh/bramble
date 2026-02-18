#include "freq_plan.h"
#include <stddef.h>

static const bramble_freq_plan_t s_plans[FREQ_REGION_COUNT] = {
    [FREQ_REGION_US915] = {
        .name              = "US915",
        .regulatory        = "FCC Part 15.247",
        .freq_start_mhz   = 902.0f,
        .freq_end_mhz     = 928.0f,
        .default_freq_mhz = 915.0f,
        .max_tx_power_dbm  = 30,
        .max_duty_cycle_pct = 100,
        .duty_cycle_enforced = false,
        .default_sf        = 9,
        .default_bw_hz     = 125000,
    },
    [FREQ_REGION_EU868] = {
        .name              = "EU868",
        .regulatory        = "ETSI EN 300.220",
        .freq_start_mhz   = 863.0f,
        .freq_end_mhz     = 870.0f,
        .default_freq_mhz = 868.1f,
        .max_tx_power_dbm  = 14,
        .max_duty_cycle_pct = 1,
        .duty_cycle_enforced = true,
        .default_sf        = 9,
        .default_bw_hz     = 125000,
    },
    [FREQ_REGION_AU915] = {
        .name              = "AU915",
        .regulatory        = "ACMA",
        .freq_start_mhz   = 915.0f,
        .freq_end_mhz     = 928.0f,
        .default_freq_mhz = 921.0f,
        .max_tx_power_dbm  = 30,
        .max_duty_cycle_pct = 100,
        .duty_cycle_enforced = false,
        .default_sf        = 9,
        .default_bw_hz     = 125000,
    },
};

const bramble_freq_plan_t *freq_plan_get_default(void)
{
#if defined(CONFIG_BRAMBLE_REGION_EU868)
    return &s_plans[FREQ_REGION_EU868];
#elif defined(CONFIG_BRAMBLE_REGION_AU915)
    return &s_plans[FREQ_REGION_AU915];
#else
    return &s_plans[FREQ_REGION_US915];
#endif
}

const bramble_freq_plan_t *freq_plan_get(bramble_freq_region_t region)
{
    if (region >= FREQ_REGION_COUNT) {
        return NULL;
    }
    return &s_plans[region];
}

bool freq_plan_valid_freq(const bramble_freq_plan_t *plan, float freq_mhz)
{
    if (!plan) return false;
    return (freq_mhz >= plan->freq_start_mhz) && (freq_mhz <= plan->freq_end_mhz);
}

bool freq_plan_valid_power(const bramble_freq_plan_t *plan, int8_t power_dbm)
{
    if (!plan) return false;
    return power_dbm <= plan->max_tx_power_dbm;
}

int8_t freq_plan_clamp_power(const bramble_freq_plan_t *plan, int8_t requested_dbm)
{
    if (!plan) return 0;
    if (requested_dbm > plan->max_tx_power_dbm) {
        return plan->max_tx_power_dbm;
    }
    return requested_dbm;
}
