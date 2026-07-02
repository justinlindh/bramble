#ifndef BEACON_POLICY_CALC_H
#define BEACON_POLICY_CALC_H

#include <stdint.h>
#include <stddef.h>

#define MAX_CHURN_HISTORY 16

typedef struct {
    uint32_t timestamp;
    uint8_t  neighbor_count;
} churn_sample_t;

/* Count neighbor-count changes within [now_ms - window_ms, now_ms] over the
 * churn ring. Slots with timestamp == 0 are unused and skipped. Pure;
 * identical to the pre-extraction mesh_task.c calculate_churn_events. */
uint8_t beacon_churn_count(const churn_sample_t *hist, size_t n, uint32_t now_ms, uint32_t window_ms);

#endif /* BEACON_POLICY_CALC_H */
