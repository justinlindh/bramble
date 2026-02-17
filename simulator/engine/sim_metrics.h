#ifndef SIM_METRICS_H
#define SIM_METRICS_H

#include <stdint.h>

typedef struct {
    uint64_t total_packets;
    uint64_t messages_sent;
    uint64_t delivered_packets;
    uint64_t dropped_packets;
    uint64_t total_latency_us;
    uint64_t latency_count;
    uint64_t messages_retried;          /* ACK retransmissions triggered */
    uint64_t messages_delivered_retry;  /* delivered after ≥1 retransmit */
    uint64_t dedup_dropped;            /* packets dropped as duplicates */
    int active_nodes;
} metrics_state_t;

void metrics_init(metrics_state_t *metrics);
void metrics_record_packet_sent(metrics_state_t *metrics);
void metrics_record_message_sent(metrics_state_t *metrics);
void metrics_record_packet_delivered(metrics_state_t *metrics, uint64_t latency_us);
void metrics_record_packet_dropped(metrics_state_t *metrics);
void metrics_update_active_nodes(metrics_state_t *metrics, int count);
double metrics_delivery_rate(const metrics_state_t *metrics);
double metrics_avg_latency_ms(const metrics_state_t *metrics);

#endif /* SIM_METRICS_H */
