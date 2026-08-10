#ifndef SIM_METRICS_H
#define SIM_METRICS_H

#include <stdint.h>

/* Per-packet-type ToA buckets for airtime_us_by_type. Indices, not wire
 * PKT_TYPE_* codes (there are more PKT_TYPE_* values than categories worth
 * breaking out); SIM_PKT_METRIC_OTHER catches anything not explicitly
 * modeled as a TX in the sim (e.g. mailbox/probe/key-exchange payloads). */
typedef enum {
    SIM_PKT_METRIC_BEACON = 0,
    SIM_PKT_METRIC_RREQ,
    SIM_PKT_METRIC_RREP,
    SIM_PKT_METRIC_RERR,
    SIM_PKT_METRIC_DATA,
    SIM_PKT_METRIC_ACK,
    SIM_PKT_METRIC_RECEIPT,
    SIM_PKT_METRIC_PROBE,
    SIM_PKT_METRIC_OTHER,
    SIM_PKT_METRIC_TYPE_COUNT,
} sim_pkt_metric_type_t;

typedef struct {
    uint64_t total_packets;
    uint64_t messages_sent;
    uint64_t delivered_packets;
    /* Phase 2 "save reactive routing" Part A: distinct scripted messages
     * whose delivery receipt made it all the way back to the true
     * ORIGINATOR (bridge.c's _handle_delivery_receipt, at the point
     * receipt.header.dest_addr == rx->addr), not just reached the
     * destination. This is Bramble's actual differentiator (confirmed
     * delivery); delivered_packets/message_delivery_rate is destination
     * REACH only (see bridge.c's "don't wait for receipt to arrive at
     * source" comment). confirmed_packets <= delivered_packets always,
     * since a receipt can only return after the destination decoded the
     * message. */
    uint64_t confirmed_packets;
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

    /* Real time-on-air (us), by packet type (sim_pkt_metric_type_t), charged
     * at the single TX chokepoint (sim_radio_broadcast) using the SAME ToA
     * value (radio_frame_airtime_us) the collision/channel model uses: one
     * source of truth, never recomputed. Sums to airtime_total_us. */
    uint64_t airtime_us_by_type[SIM_PKT_METRIC_TYPE_COUNT];
} metrics_state_t;

void metrics_init(metrics_state_t* metrics);
void metrics_record_packet_sent(metrics_state_t* metrics);
void metrics_record_message_sent(metrics_state_t* metrics);
void metrics_record_packet_delivered(metrics_state_t* metrics, uint64_t latency_us);
/* Records a distinct scripted message's delivery receipt reaching the true
 * originator; see confirmed_packets' doc comment above. Caller
 * (bridge_msg_track_confirm) is responsible for de-duplicating so a given
 * packet_id only increments this once. */
void metrics_record_packet_confirmed(metrics_state_t* metrics);
void metrics_record_packet_dropped(metrics_state_t* metrics);
void metrics_record_beacon_sent(metrics_state_t* metrics);
void metrics_record_rreq_sent(metrics_state_t* metrics);
void metrics_record_rrep_sent(metrics_state_t* metrics);
void metrics_update_active_nodes(metrics_state_t* metrics, int count);
/* Charges airtime_us of real time-on-air (the exact value the radio medium
 * model computed for this transmission) to the bucket for pkt_type. Called
 * once per actual (post-budget-gate) transmission, from sim_radio_broadcast. */
void metrics_record_tx_airtime(metrics_state_t* metrics, uint8_t pkt_type, uint32_t airtime_us);
double metrics_avg_latency_ms(const metrics_state_t* metrics);
/* ToA-weighted control-plane share: ToA(beacon+RREQ+RREP+RERR) / ToA(all).
 * This is the HONEST replacement for the packet-count ratio this function
 * used to compute; see metrics_control_packet_pct for that old semantic,
 * kept under its own honest name. */
double metrics_control_airtime_pct(const metrics_state_t* metrics);
/* Packet-COUNT control-plane share (beacon+RREQ+RREP sent / total packets
 * sent): the exact formula metrics_control_airtime_pct used to compute
 * before it was fixed to be ToA-weighted. Kept for continuity under an
 * honest name; note it does not include RERR, unlike the ToA-weighted
 * version, since it is a frozen copy of the old (mislabeled) formula. */
double metrics_control_packet_pct(const metrics_state_t* metrics);

#endif /* SIM_METRICS_H */
