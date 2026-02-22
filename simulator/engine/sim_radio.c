#include "sim_radio.h"
#include "sim_emitter.h"
#include <math.h>
#include <string.h>

void radio_config_init(radio_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->range = 150.0f;
    config->loss_pct = 0.0f;
    config->propagation_speed_ms_per_unit = 0.1f;
}

float radio_distance(const sim_node_t *a, const sim_node_t *b) {
    float dx = a->x - b->x;
    float dy = a->y - b->y;
    return sqrtf(dx * dx + dy * dy);
}

int8_t radio_compute_rssi(const radio_config_t *config, float distance) {
    if (distance > config->range)
        return -100;
    
    /* RSSI model: -40 at 0, -100 at range */
    float ratio = distance / config->range;
    int8_t rssi = (int8_t)(-40.0f - 60.0f * ratio);
    
    /* Clamp to [-100, -40] */
    if (rssi < -100) rssi = -100;
    if (rssi > -40) rssi = -40;
    
    return rssi;
}

bool radio_can_receive(const radio_config_t *config, const sim_node_t *tx, const sim_node_t *rx, pcg32_state_t *rng) {
    /* Both must be active */
    if (!tx->active || !rx->active)
        return false;
    
    /* Check distance */
    float dist = radio_distance(tx, rx);
    if (dist > config->range)
        return false;
    
    /* Check interference */
    if (radio_in_interference(config, rx))
        return false;
    
    /* Random packet loss */
    if (config->loss_pct > 0.0f) {
        float roll = pcg32_float(rng) * 100.0f;
        if (roll < config->loss_pct)
            return false;
    }
    
    return true;
}

uint64_t radio_propagation_delay_us(const radio_config_t *config, float distance) {
    float delay_ms = distance * config->propagation_speed_ms_per_unit;
    return (uint64_t)(delay_ms * 1000.0f);
}

int radio_add_interference_zone(radio_config_t *config, float center_x, float center_y, float radius) {
    if (config->zone_count >= MAX_INTERFERENCE_ZONES)
        return -1;
    
    int idx = config->zone_count++;
    config->zones[idx].center_x = center_x;
    config->zones[idx].center_y = center_y;
    config->zones[idx].radius = radius;
    config->zones[idx].active = true;
    return idx;
}

void radio_clear_interference_zone(radio_config_t *config, int index) {
    if (index >= 0 && index < config->zone_count)
        config->zones[index].active = false;
}

bool radio_in_interference(const radio_config_t *config, const sim_node_t *node) {
    for (int i = 0; i < config->zone_count; i++) {
        if (!config->zones[i].active)
            continue;
        
        float dx = node->x - config->zones[i].center_x;
        float dy = node->y - config->zones[i].center_y;
        float dist = sqrtf(dx * dx + dy * dy);
        
        if (dist <= config->zones[i].radius)
            return true;
    }
    return false;
}

/*
 * sim_radio_broadcast — transmit pkt from tx_node.
 *
 * For each other active node in range, schedules an EVT_RECEIVE_PACKET
 * event with propagation delay.  Out-of-range or loss-dropped nodes get
 * a packet_dropped event.  Emits one packet_sent event for the transmitter.
 */
void sim_radio_broadcast(
    sim_node_t *tx_node,
    const outbound_packet_t *pkt,
    node_array_t *nodes,
    radio_config_t *radio,
    pcg32_state_t *rng,
    event_queue_t *events,
    metrics_state_t *metrics,
    uint64_t now_us)
{
    /* Emit packet_sent for visualization */
    emit_packet_sent_typed(stdout, now_us, tx_node->id,
                           tx_node->addr, pkt->dest_addr,
                           pkt->len, pkt->pkt_type);
    metrics_record_packet_sent(metrics);
    tx_node->packets_sent++;
    
    /* Record control packet metrics */
    if (pkt->pkt_type == PKT_TYPE_BEACON) {
        metrics_record_beacon_sent(metrics);
    } else if (pkt->pkt_type == PKT_TYPE_RREQ) {
        metrics_record_rreq_sent(metrics);
    } else if (pkt->pkt_type == PKT_TYPE_RREP) {
        metrics_record_rrep_sent(metrics);
    }

    /* Deliver to all nodes in range */
    for (int i = 0; i < nodes->count; i++) {
        sim_node_t *rx = &nodes->nodes[i];
        if (rx == tx_node || !rx->active) continue;

        /* For unicast, skip nodes that are not the target */
        if (!pkt->is_broadcast && pkt->dest_addr != rx->addr) continue;

        if (!radio_can_receive(radio, tx_node, rx, rng)) {
            /* Only emit drop for unicast targets and in-range nodes
             * (for broadcast we silently skip out-of-range neighbors) */
            float dist = radio_distance(tx_node, rx);
            if (!pkt->is_broadcast && pkt->dest_addr == rx->addr) {
                emit_packet_dropped(stdout, now_us, rx->id, "radio_loss");
                metrics_record_packet_dropped(metrics);
            } else if (pkt->is_broadcast && dist <= radio->range) {
                /* In-range but dropped due to loss_pct or interference */
                metrics_record_packet_dropped(metrics);
            }
            continue;
        }

        float dist     = radio_distance(tx_node, rx);
        uint64_t delay = radio_propagation_delay_us(radio, dist);
        int8_t rssi    = radio_compute_rssi(radio, dist);

        /* SNR = RSSI - noise_floor; noise_floor = -120 dBm (typical LoRa) */
        #define NOISE_FLOOR_DBM (-120)
        int snr_raw = (int)rssi - NOISE_FLOOR_DBM;
        /* Add ±2 dB random jitter for realism */
        float jitter = (pcg32_float(rng) - 0.5f) * 4.0f; /* -2.0 to +2.0 */
        int snr_jittered = snr_raw + (int)jitter;
        if (snr_jittered < 0) snr_jittered = 0;
        if (snr_jittered > 127) snr_jittered = 127;
        int8_t snr = (int8_t)snr_jittered;

        /* Schedule EVT_RECEIVE_PACKET for this node */
        sim_event_t recv_evt;
        memset(&recv_evt, 0, sizeof(recv_evt));
        recv_evt.type                    = EVT_RECEIVE_PACKET;
        recv_evt.timestamp_us            = now_us + delay;
        recv_evt.data.packet.src_addr    = tx_node->addr;
        recv_evt.data.packet.dest_addr   = rx->addr;
        recv_evt.data.packet.rssi        = rssi;
        recv_evt.data.packet.snr         = snr;
        recv_evt.data.packet.len         = pkt->len;
        memcpy(recv_evt.data.packet.data, pkt->data, pkt->len);

        event_queue_push(events, &recv_evt);
    }
}
