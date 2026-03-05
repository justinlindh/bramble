#ifndef BRAMBLE_FREQ_PLAN_H
#define BRAMBLE_FREQ_PLAN_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FREQ_REGION_US915 = 0,
    FREQ_REGION_EU868,
    FREQ_REGION_AU915,
    FREQ_REGION_COUNT
} bramble_freq_region_t;

typedef struct {
    const char* name;
    const char* regulatory; // "FCC Part 15.247", "ETSI EN 300.220", etc.
    float freq_start_mhz;
    float freq_end_mhz;
    float default_freq_mhz;
    int8_t max_tx_power_dbm;
    uint8_t max_duty_cycle_pct; // 100 = no limit (US), 1 = 1% (EU)
    bool duty_cycle_enforced;   // hard enforce vs advisory
    uint8_t default_sf;
    uint32_t default_bw_hz;
} bramble_freq_plan_t;

// Get the compile-time default region's plan
const bramble_freq_plan_t* freq_plan_get_default(void);

// Get a plan by region
const bramble_freq_plan_t* freq_plan_get(bramble_freq_region_t region);

// Validate: is this frequency within the plan's band?
bool freq_plan_valid_freq(const bramble_freq_plan_t* plan, float freq_mhz);

// Validate: is this TX power within regulatory limits?
bool freq_plan_valid_power(const bramble_freq_plan_t* plan, int8_t power_dbm);

// Clamp TX power to regulatory max
int8_t freq_plan_clamp_power(const bramble_freq_plan_t* plan, int8_t requested_dbm);

#endif
