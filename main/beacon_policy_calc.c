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
