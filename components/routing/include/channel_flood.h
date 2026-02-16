#ifndef BRAMBLE_CHANNEL_FLOOD_H
#define BRAMBLE_CHANNEL_FLOOD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     should_relay;
    uint8_t  new_hop_limit;
    uint32_t jitter_ms;
} flood_decision_t;

flood_decision_t channel_flood_decide(uint8_t hop_limit, uint32_t packet_id, void *dedup_context);

#endif
