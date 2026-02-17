#ifndef SIM_SCENARIO_H
#define SIM_SCENARIO_H

#include <stdint.h>
#include <stdbool.h>
#include "sim_event.h"
#include "sim_node.h"
#include "sim_radio.h"
#include "sim_random.h"

typedef struct {
    char name[64];
    bool deterministic;
    uint64_t seed;
    uint64_t duration_us;
} scenario_metadata_t;

typedef struct {
    scenario_metadata_t metadata;
    node_array_t *nodes;
    radio_config_t *radio;
    event_queue_t *events;
    pcg32_state_t *rng;
} scenario_t;

bool scenario_load_file(const char *path, scenario_t *scenario);

#endif /* SIM_SCENARIO_H */
