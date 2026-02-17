#include "sim_radio.h"
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
