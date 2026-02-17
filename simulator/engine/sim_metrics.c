#include "sim_metrics.h"
#include <string.h>

void metrics_init(metrics_state_t *metrics) {
    memset(metrics, 0, sizeof(*metrics));
}

void metrics_record_packet_sent(metrics_state_t *metrics) {
    metrics->total_packets++;
}

void metrics_record_packet_delivered(metrics_state_t *metrics, uint64_t latency_us) {
    metrics->delivered_packets++;
    metrics->total_latency_us += latency_us;
    metrics->latency_count++;
}

void metrics_record_packet_dropped(metrics_state_t *metrics) {
    metrics->dropped_packets++;
}

void metrics_update_active_nodes(metrics_state_t *metrics, int count) {
    metrics->active_nodes = count;
}

double metrics_delivery_rate(const metrics_state_t *metrics) {
    if (metrics->total_packets == 0)
        return 0.0;
    return (double)metrics->delivered_packets / (double)metrics->total_packets;
}

double metrics_avg_latency_ms(const metrics_state_t *metrics) {
    if (metrics->latency_count == 0)
        return 0.0;
    return (double)metrics->total_latency_us / (double)metrics->latency_count / 1000.0;
}
