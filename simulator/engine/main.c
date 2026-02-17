#include "../../test/stubs/esp_stubs.h"

#include "sim_event.h"
#include "sim_random.h"
#include "sim_emitter.h"
#include "sim_node.h"
#include "sim_radio.h"
#include "sim_scenario.h"
#include "sim_metrics.h"
#include "sim_anomaly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global simulation state */
static node_array_t g_nodes;
static radio_config_t g_radio;
static event_queue_t g_events;
static pcg32_state_t g_rng;
static uint64_t g_sim_time_us = 0;
static metrics_state_t g_metrics;

/* Bramble expects this function */
uint32_t sim_get_time_ms(void) {
    return (uint32_t)(g_sim_time_us / 1000);
}

/* Include Bramble component implementations directly */
#include "../../components/routing/routing.c"
#include "../../components/routing/discovery.c"
#include "../../components/routing/forwarding.c"
#include "../../components/packet/packet.c"

static void handle_event(sim_event_t *event) {
    switch (event->type) {
        case EVT_NODE_MOVE: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (node) {
                node_move(node, event->data.node.x, event->data.node.y);
                emit_node_moved(stdout, event->timestamp_us, node->id, node->x, node->y);
            }
            break;
        }

        case EVT_NODE_LEAVE: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (node) {
                node_deactivate(node);
                emit_node_left(stdout, event->timestamp_us, node->id);
            }
            break;
        }

        case EVT_NODE_JOIN: {
            sim_node_t *node = node_array_find_by_id(&g_nodes, event->data.node.node_id);
            if (node) {
                node_activate(node);
                emit_node_joined(stdout, event->timestamp_us, node->id, node->addr);
            }
            break;
        }

        case EVT_INTERFERENCE_START: {
            int zone_idx = radio_add_interference_zone(&g_radio, 
                event->data.interference.center_x,
                event->data.interference.center_y,
                event->data.interference.radius);
            fprintf(stderr, "Interference zone %d started at (%.1f, %.1f) radius %.1f\n",
                    zone_idx, event->data.interference.center_x, 
                    event->data.interference.center_y, event->data.interference.radius);
            break;
        }

        case EVT_INTERFERENCE_END: {
            if (event->data.interference.zone_index >= 0) {
                radio_clear_interference_zone(&g_radio, event->data.interference.zone_index);
                fprintf(stderr, "Interference zone %d ended\n", event->data.interference.zone_index);
            }
            break;
        }

        case EVT_METRICS_TICK: {
            /* Count active nodes */
            int active = 0;
            for (int i = 0; i < g_nodes.count; i++) {
                if (g_nodes.nodes[i].active)
                    active++;
            }
            metrics_update_active_nodes(&g_metrics, active);
            
            emit_metrics(stdout, event->timestamp_us, active,
                        g_metrics.total_packets, g_metrics.delivered_packets,
                        g_metrics.dropped_packets, metrics_avg_latency_ms(&g_metrics));
            break;
        }

        case EVT_GENERATE_MESSAGE: {
            /* TODO: Implement message generation and routing */
            fprintf(stderr, "TODO: Generate message from %s to 0x%08X\n",
                    event->data.node.node_id, event->data.node.addr);
            break;
        }

        default:
            fprintf(stderr, "Unhandled event type %d\n", event->type);
            break;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <scenario.json>\n", argv[0]);
        return 1;
    }

    /* Initialize simulation state */
    node_array_init(&g_nodes);
    event_queue_init(&g_events);
    metrics_init(&g_metrics);

    /* Load scenario */
    scenario_t scenario = {
        .nodes = &g_nodes,
        .radio = &g_radio,
        .events = &g_events,
        .rng = &g_rng,
    };

    if (!scenario_load_file(argv[1], &scenario)) {
        fprintf(stderr, "Error: failed to load scenario\n");
        return 1;
    }

    pcg32_seed(&g_rng, scenario.metadata.seed);

    fprintf(stderr, "Loaded scenario '%s'\n", scenario.metadata.name);
    fprintf(stderr, "  Duration: %llu ms\n", (unsigned long long)(scenario.metadata.duration_us / 1000));
    fprintf(stderr, "  Nodes: %d\n", g_nodes.count);
    fprintf(stderr, "  Events: %d\n", event_queue_count(&g_events));
    fprintf(stderr, "  Mode: %s\n", scenario.metadata.deterministic ? "deterministic" : "stochastic");
    fprintf(stderr, "  Seed: %llu\n", (unsigned long long)scenario.metadata.seed);
    fprintf(stderr, "\nStarting simulation...\n\n");

    /* Main event loop */
    sim_event_t event;
    while (event_queue_pop(&g_events, &event)) {
        if (event.timestamp_us > scenario.metadata.duration_us)
            break;

        g_sim_time_us = event.timestamp_us;
        handle_event(&event);
    }

    fprintf(stderr, "\nSimulation complete.\n");
    fprintf(stderr, "Final metrics:\n");
    fprintf(stderr, "  Total packets: %llu\n", (unsigned long long)g_metrics.total_packets);
    fprintf(stderr, "  Delivered: %llu\n", (unsigned long long)g_metrics.delivered_packets);
    fprintf(stderr, "  Dropped: %llu\n", (unsigned long long)g_metrics.dropped_packets);
    fprintf(stderr, "  Delivery rate: %.2f%%\n", metrics_delivery_rate(&g_metrics) * 100.0);
    fprintf(stderr, "  Avg latency: %.3f ms\n", metrics_avg_latency_ms(&g_metrics));

    return 0;
}
