#ifndef SIM_RADIO_H
#define SIM_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "sim_node.h"
#include "sim_random.h"
#include "sim_event.h"
#include "sim_metrics.h"

#define MAX_INTERFERENCE_ZONES 16

typedef struct {
    float center_x;
    float center_y;
    float radius;
    bool active;
} interference_zone_t;

typedef struct {
    float range;
    float loss_pct;
    float propagation_speed_ms_per_unit;
    interference_zone_t zones[MAX_INTERFERENCE_ZONES];
    int zone_count;
} radio_config_t;

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
 * sim_radio_broadcast — transmit a packet from tx_node to all reachable nodes.
 * Schedules EVT_RECEIVE_PACKET events with propagation delay.
 * Emits packet_sent + packet_dropped events.
 */
void sim_radio_broadcast(sim_node_t* tx_node, const outbound_packet_t* pkt, node_array_t* nodes,
                         radio_config_t* radio, pcg32_state_t* rng, event_queue_t* events,
                         metrics_state_t* metrics, uint64_t now_us);

#endif /* SIM_RADIO_H */
