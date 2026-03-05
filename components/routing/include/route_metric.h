#ifndef BRAMBLE_ROUTE_METRIC_H
#define BRAMBLE_ROUTE_METRIC_H

#include <stdint.h>
#include <stdbool.h>

/* Weights (must sum to 256 for integer math) */
#define METRIC_WEIGHT_LINK_QUALITY 102 /* 40% */
#define METRIC_WEIGHT_DELIVERY_RATE 77 /* 30% */
#define METRIC_WEIGHT_AIRTIME 51       /* 20% */
#define METRIC_WEIGHT_LATENCY 26       /* 10% */

/* Hysteresis: minimum metric improvement to switch routes */
#define METRIC_HYSTERESIS_THRESHOLD 15
#define METRIC_SWITCH_COOLDOWN_MS 10000 /* 10s between route switches */

/* Compute composite metric (0-255, higher = better) */
uint8_t route_metric_compute(uint8_t link_quality, uint8_t delivery_rate, uint8_t airtime_remaining,
                             uint16_t latency_ms);

/* Update delivery rate EMA (call on ACK received or timeout) */
uint8_t route_metric_update_delivery(uint8_t current_rate, bool success);

/* Update latency EMA */
uint16_t route_metric_update_latency(uint16_t current_avg, uint16_t new_sample);

/* Check if new metric is significantly better (hysteresis) */
bool route_metric_should_switch(uint8_t current_metric, uint8_t new_metric, uint32_t last_switch_ms,
                                uint32_t now_ms);

/* Convert airtime budget remaining to 0-255 scale */
uint8_t route_metric_airtime_score(uint32_t remaining_ms, uint32_t max_ms);

#endif
