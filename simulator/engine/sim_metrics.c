#include "sim_metrics.h"
#include "../../components/packet/include/packet.h"
#include <string.h>

void metrics_init(metrics_state_t* metrics) { memset(metrics, 0, sizeof(*metrics)); }

void metrics_record_packet_sent(metrics_state_t* metrics) { metrics->total_packets++; }

void metrics_record_message_sent(metrics_state_t* metrics) { metrics->messages_sent++; }

void metrics_record_packet_delivered(metrics_state_t* metrics, uint64_t latency_us) {
    metrics->delivered_packets++;
    metrics->total_latency_us += latency_us;
    metrics->latency_count++;
}

void metrics_record_packet_confirmed(metrics_state_t* metrics) { metrics->confirmed_packets++; }

void metrics_record_packet_dropped(metrics_state_t* metrics) { metrics->dropped_packets++; }

void metrics_update_active_nodes(metrics_state_t* metrics, int count) {
    metrics->active_nodes = count;
}

double metrics_avg_latency_ms(const metrics_state_t* metrics) {
    if (metrics->latency_count == 0)
        return 0.0;
    return (double)metrics->total_latency_us / (double)metrics->latency_count / 1000.0;
}

void metrics_record_beacon_sent(metrics_state_t* metrics) { metrics->beacons_sent++; }

void metrics_record_rreq_sent(metrics_state_t* metrics) { metrics->rreqs_sent++; }

void metrics_record_rrep_sent(metrics_state_t* metrics) { metrics->rreps_sent++; }

static sim_pkt_metric_type_t pkt_metric_type(uint8_t pkt_type) {
    switch (pkt_type) {
    case PKT_TYPE_BEACON:
        return SIM_PKT_METRIC_BEACON;
    case PKT_TYPE_RREQ:
        return SIM_PKT_METRIC_RREQ;
    case PKT_TYPE_RREP:
        return SIM_PKT_METRIC_RREP;
    case PKT_TYPE_RERR:
        return SIM_PKT_METRIC_RERR;
    case PKT_TYPE_DATA:
        return SIM_PKT_METRIC_DATA;
    case PKT_TYPE_ACK:
        return SIM_PKT_METRIC_ACK;
    case PKT_TYPE_DELIVERY_RECEIPT:
        return SIM_PKT_METRIC_RECEIPT;
    case PKT_TYPE_PROBE:
    case PKT_TYPE_PROBE_ACK:
        return SIM_PKT_METRIC_PROBE;
    default:
        return SIM_PKT_METRIC_OTHER;
    }
}

void metrics_record_tx_airtime(metrics_state_t* metrics, uint8_t pkt_type, uint32_t airtime_us) {
    metrics->airtime_us_by_type[pkt_metric_type(pkt_type)] += airtime_us;
}

double metrics_control_airtime_pct(const metrics_state_t* metrics) {
    if (metrics->airtime_total_us == 0)
        return 0.0;
    uint64_t control_us = metrics->airtime_us_by_type[SIM_PKT_METRIC_BEACON] +
                          metrics->airtime_us_by_type[SIM_PKT_METRIC_RREQ] +
                          metrics->airtime_us_by_type[SIM_PKT_METRIC_RREP] +
                          metrics->airtime_us_by_type[SIM_PKT_METRIC_RERR];
    return (double)control_us / (double)metrics->airtime_total_us * 100.0;
}

double metrics_control_packet_pct(const metrics_state_t* metrics) {
    if (metrics->total_packets == 0)
        return 0.0;
    uint64_t control_packets = metrics->beacons_sent + metrics->rreqs_sent + metrics->rreps_sent;
    return (double)control_packets / (double)metrics->total_packets * 100.0;
}
