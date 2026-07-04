#ifndef SIM_RADIO_H
#define SIM_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "sim_node.h"
#include "sim_random.h"
#include "sim_event.h"
#include "sim_metrics.h"

#define MAX_INTERFERENCE_ZONES 16

/* Channel occupancy log capacity. Entries older than the retention window are
 * pruned on insert, so this only needs to hold the transmissions that can
 * still overlap a packet in flight. */
#define MAX_CHANNEL_TX 1024
#define CHANNEL_LOG_RETENTION_US 10000000ULL /* 10 s */

typedef struct {
    float center_x;
    float center_y;
    float radius;
    bool active;
} interference_zone_t;

/* One transmission's occupancy of the shared medium */
typedef struct {
    uint32_t tx_addr;
    float tx_x;
    float tx_y;
    uint64_t start_us;
    uint64_t end_us;
} channel_tx_t;

typedef struct {
    channel_tx_t entries[MAX_CHANNEL_TX];
    int count;
    uint64_t overflow_drops; /* oldest entries evicted while still in retention */
} channel_log_t;

/* Outcome of a reception attempt under the collision model */
typedef enum {
    RADIO_RX_OK = 0,      /* no overlapping transmission */
    RADIO_RX_COLLISION,   /* destroyed by an overlapping transmission */
    RADIO_RX_HALF_DUPLEX, /* receiver was transmitting during the packet */
    RADIO_RX_CAPTURED,    /* overlapped, but won via the capture effect */
} radio_rx_outcome_t;

/* Tagged so sim_node.h can forward-declare radio_config_t (node_tick needs a
 * pointer to it to compute beacon/RREQ airtime; sim_node.h cannot include
 * this header back, since this header already includes sim_node.h). */
typedef struct radio_config {
    float range;
    float loss_pct;
    float propagation_speed_ms_per_unit;
    interference_zone_t zones[MAX_INTERFERENCE_ZONES];
    int zone_count;

    /* LoRa PHY parameters used for time-on-air and the RSSI gradient.
     * Defaults mirror the firmware's RADIO_PROFILE_LONG_RANGE
     * (components/radio/radio_esp.c): SF10, 125 kHz, CR 4/5, 22 dBm. */
    uint8_t sf;
    uint32_t bw_hz;
    uint8_t cr; /* 1..4 => 4/5..4/8 */
    int8_t tx_power_dbm;

    /* Collision model (see simulator/README.md "Radio model"). */
    bool collisions_enabled;
    bool lbt_enabled;      /* listen-before-talk, mirrors firmware transmit_packet */
    float capture_db;      /* co-SF capture threshold, default 6 dB */
    float path_loss_exp;   /* log-distance path loss exponent n */
    float path_loss_d0_db; /* path loss at d0 = 1 grid unit (10 m) */
    channel_log_t channel; /* occupancy log of recent transmissions */
} radio_config_t;

/* Listen-before-talk parameters, mirroring main/mesh_task.c transmit_packet
 * (LBT_MAX_ATTEMPTS / LBT_BACKOFF_BASE_MS / LBT_BACKOFF_MAX_MS). */
#define SIM_LBT_MAX_ATTEMPTS 3
#define SIM_LBT_BACKOFF_BASE_MS 50u
#define SIM_LBT_BACKOFF_MAX_MS 300u

void radio_config_init(radio_config_t* config);
float radio_distance(const sim_node_t* a, const sim_node_t* b);
int8_t radio_compute_rssi(const radio_config_t* config, float distance);
bool radio_can_receive(const radio_config_t* config, const sim_node_t* tx, const sim_node_t* rx,
                       pcg32_state_t* rng);
uint64_t radio_propagation_delay_us(const radio_config_t* config, float distance);
int radio_add_interference_zone(radio_config_t* config, float center_x, float center_y,
                                float radius);
void radio_clear_interference_zone(radio_config_t* config, int index);
bool radio_in_interference(const radio_config_t* config, const sim_node_t* node);

/*
 * radio_frame_airtime_us: real LoRa time-on-air for a frame of frame_bytes
 * PHY payload bytes, using the configured SF/BW/CR. Delegates to the firmware's
 * bramble_calculate_airtime_us (components/radio/radio_airtime.c).
 */
uint32_t radio_frame_airtime_us(const radio_config_t* config, uint16_t frame_bytes);

/*
 * radio_frame_airtime_ms: radio_frame_airtime_us rounded up to whole
 * milliseconds (minimum 1 ms), the unit the airtime budget accounts in.
 * Single source of truth for the us->ms rounding used at every budget-gated
 * TX site (never undercount airtime).
 */
uint32_t radio_frame_airtime_ms(const radio_config_t* config, uint16_t frame_bytes);

/*
 * radio_preamble_us: duration of the LoRa preamble (programmed symbols + 4.25
 * sync symbols) for the configured SF/BW. Used as the capture re-sync window.
 */
uint64_t radio_preamble_us(const radio_config_t* config);

/*
 * radio_check_reception: evaluate a delivery under the collision model.
 * Called at delivery time (end of the packet's air window), when every
 * transmission that could overlap it is already in the channel log.
 * Returns RADIO_RX_OK / RADIO_RX_CAPTURED when the packet is receivable.
 */
radio_rx_outcome_t radio_check_reception(const radio_config_t* config, const sim_node_t* rx,
                                         const packet_event_data_t* pkt);

/*
 * sim_radio_broadcast: transmit a packet from tx_node to all reachable nodes.
 * Computes real time-on-air, serializes the node's transmissions (half-duplex
 * radio cannot start a new TX while one is in progress), records the channel
 * occupancy window, and schedules EVT_RECEIVE_PACKET at end-of-packet plus
 * propagation delay. Emits packet_sent + packet_dropped events.
 */
void sim_radio_broadcast(sim_node_t* tx_node, const outbound_packet_t* pkt, node_array_t* nodes,
                         radio_config_t* radio, pcg32_state_t* rng, event_queue_t* events,
                         metrics_state_t* metrics, uint64_t now_us);

#endif /* SIM_RADIO_H */
