#include "sim_metrics.h"
#include <string.h>

void metrics_init(metrics_state_t *metrics) {
    memset(metrics, 0, sizeof(*metrics));
}

void metrics_record_packet_sent(metrics_state_t *metrics) {
    metrics->total_packets++;
}

void metrics_record_message_sent(metrics_state_t *metrics) {
    metrics->messages_sent++;
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

void metrics_record_beacon_sent(metrics_state_t *metrics) {
    metrics->beacons_sent++;
}

void metrics_record_rreq_sent(metrics_state_t *metrics) {
    metrics->rreqs_sent++;
}

void metrics_record_rrep_sent(metrics_state_t *metrics) {
    metrics->rreps_sent++;
}

double metrics_control_airtime_pct(const metrics_state_t *metrics) {
    if (metrics->total_packets == 0)
        return 0.0;
    uint64_t control_packets = metrics->beacons_sent + metrics->rreqs_sent + metrics->rreps_sent;
    return (double)control_packets / (double)metrics->total_packets * 100.0;
}
