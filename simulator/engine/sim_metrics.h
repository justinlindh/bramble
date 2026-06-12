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
    uint64_t messages_retried;         /* ACK retransmissions triggered */
    uint64_t messages_delivered_retry; /* delivered after ≥1 retransmit */
    uint64_t dedup_dropped;            /* packets dropped as duplicates */
    uint64_t airtime_deferred;         /* packets deferred due to airtime budget */
    uint64_t fragments_sent;           /* fragment packets sent */
    uint64_t fragments_reassembled;    /* messages fully reassembled */
    uint64_t reassembly_timeout;       /* reassemblies that timed out */
    uint64_t crypto_encrypted;         /* packets encrypted */
    uint64_t crypto_decrypted;         /* packets decrypted */
    uint64_t crypto_auth_failed;       /* packets with auth failure */
    uint64_t beacons_sent;             /* total beacons transmitted (control airtime) */
    uint64_t rreqs_sent;               /* total RREQs transmitted (control airtime) */
    uint64_t rreps_sent;               /* total RREPs transmitted (control airtime) */
    uint64_t collisions;               /* receptions destroyed by overlap */
    uint64_t half_duplex_drops;        /* receptions missed while transmitting */
    uint64_t capture_wins;             /* receptions that survived overlap via capture */
    uint64_t lbt_backoffs;             /* listen-before-talk busy detections */
    uint64_t receptions_ok;            /* receptions that passed the collision model */
    uint64_t airtime_total_us;         /* sum of real time-on-air across all TX */
    int active_nodes;
} metrics_state_t;

void metrics_init(metrics_state_t* metrics);
void metrics_record_packet_sent(metrics_state_t* metrics);
void metrics_record_message_sent(metrics_state_t* metrics);
void metrics_record_packet_delivered(metrics_state_t* metrics, uint64_t latency_us);
void metrics_record_packet_dropped(metrics_state_t* metrics);
void metrics_record_beacon_sent(metrics_state_t* metrics);
void metrics_record_rreq_sent(metrics_state_t* metrics);
void metrics_record_rrep_sent(metrics_state_t* metrics);
void metrics_update_active_nodes(metrics_state_t* metrics, int count);
double metrics_delivery_rate(const metrics_state_t* metrics);
double metrics_avg_latency_ms(const metrics_state_t* metrics);
double metrics_control_airtime_pct(const metrics_state_t* metrics);

#endif /* SIM_METRICS_H */
