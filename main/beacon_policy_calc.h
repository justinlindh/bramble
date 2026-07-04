#ifndef BEACON_POLICY_CALC_H
#define BEACON_POLICY_CALC_H

#include <stdint.h>
#include <stddef.h>

#define MAX_CHURN_HISTORY 16

typedef struct {
    uint32_t timestamp;
    uint8_t neighbor_count;
} churn_sample_t;

/* Count neighbor-count changes within [now_ms - window_ms, now_ms] over the
 * churn ring. Slots with timestamp == 0 are unused and skipped. Pure;
 * identical to the pre-extraction mesh_task.c calculate_churn_events. */
uint8_t beacon_churn_count(const churn_sample_t* hist, size_t n, uint32_t now_ms,
                           uint32_t window_ms);

typedef struct {
    uint32_t interval_ms;
    int in_backoff;      /* 0/1 */
    int adaptive_active; /* 0/1: adaptive branch ran */
} beacon_interval_decision_t;

/* Select the adaptive beacon interval. Pure: no status writes, no logging.
 * mode_is_adaptive is (policy.mode == BEACON_MODE_ADAPTIVE). Mirrors the
 * decision branches of the pre-extraction compute_adaptive_beacon_interval. */
beacon_interval_decision_t
beacon_interval_decide(int enabled, int mode_is_adaptive, uint32_t base_interval_ms,
                       uint32_t min_interval_ms, uint32_t max_interval_ms, uint8_t dense_threshold,
                       uint8_t churn_threshold, uint8_t neighbor_count, uint8_t churn_events);

#endif /* BEACON_POLICY_CALC_H */
