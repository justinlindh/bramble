#include "beacon_policy_calc.h"

uint8_t beacon_churn_count(const churn_sample_t *hist, size_t n, uint32_t now_ms, uint32_t window_ms) {
    uint8_t events = 0;
    uint8_t last_count = 0xff;
    int first = 1;

    for (size_t i = 0; i < n; i++) {
        if (hist[i].timestamp == 0) continue;
        if ((now_ms - hist[i].timestamp) > window_ms) continue;

        if (first) {
            last_count = hist[i].neighbor_count;
            first = 0;
        } else if (hist[i].neighbor_count != last_count) {
            events++;
            last_count = hist[i].neighbor_count;
        }
    }
    return events;
}

beacon_interval_decision_t beacon_interval_decide(
    int enabled, int mode_is_adaptive,
    uint32_t base_interval_ms, uint32_t min_interval_ms, uint32_t max_interval_ms,
    uint8_t dense_threshold, uint8_t churn_threshold,
    uint8_t neighbor_count, uint8_t churn_events) {
    beacon_interval_decision_t d;

    if (!enabled || !mode_is_adaptive) {
        d.interval_ms = base_interval_ms;
        d.in_backoff = 0;
        d.adaptive_active = 0;
        return d;
    }

    d.adaptive_active = 1;

    if (neighbor_count >= dense_threshold) {
        d.interval_ms = max_interval_ms;
        d.in_backoff = 1;
    } else if (churn_events >= churn_threshold) {
        d.interval_ms = min_interval_ms;
        d.in_backoff = 0;
    } else {
        d.interval_ms = base_interval_ms;
        d.in_backoff = 0;
    }
    return d;
}
