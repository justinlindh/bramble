#include "route_metric.h"

uint8_t route_metric_compute(uint8_t link_quality, uint8_t delivery_rate, uint8_t airtime_remaining,
                             uint16_t latency_ms) {
    /* Convert latency to score: lower latency = higher score, cap at 1020ms */
    uint16_t lat_capped = latency_ms / 4;
    if (lat_capped > 255)
        lat_capped = 255;
    uint8_t latency_score = 255 - (uint8_t)lat_capped;

    uint32_t composite = (uint32_t)link_quality * METRIC_WEIGHT_LINK_QUALITY +
                         (uint32_t)delivery_rate * METRIC_WEIGHT_DELIVERY_RATE +
                         (uint32_t)airtime_remaining * METRIC_WEIGHT_AIRTIME +
                         (uint32_t)latency_score * METRIC_WEIGHT_LATENCY;

    return (uint8_t)(composite / 256);
}

uint8_t route_metric_update_delivery(uint8_t current_rate, bool success) {
    /* EMA alpha=1/8: new = current - current/8 + (success ? 255/8 : 0) */
    uint8_t result = current_rate - current_rate / 8;
    if (success)
        result += 255 / 8; /* 31 */
    return result;
}

uint16_t route_metric_update_latency(uint16_t current_avg, uint16_t new_sample) {
    /* EMA alpha=1/4: new = current - current/4 + sample/4 */
    return current_avg - current_avg / 4 + new_sample / 4;
}

bool route_metric_should_switch(uint8_t current_metric, uint8_t new_metric, uint32_t last_switch_ms,
                                uint32_t now_ms) {
    if (now_ms - last_switch_ms < METRIC_SWITCH_COOLDOWN_MS)
        return false;
    return new_metric > current_metric + METRIC_HYSTERESIS_THRESHOLD;
}

uint8_t route_metric_airtime_score(uint32_t remaining_ms, uint32_t max_ms) {
    if (max_ms == 0)
        return 0;
    if (remaining_ms >= max_ms)
        return 255;
    return (uint8_t)((remaining_ms * 255) / max_ms);
}
