#include "sim_scenario.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool load_nodes(cJSON *nodes_json, node_array_t *nodes) {
    if (!cJSON_IsArray(nodes_json))
        return false;

    uint32_t addr = 0x01000000;  /* Auto-assign addresses */
    cJSON *node_json = NULL;
    cJSON_ArrayForEach(node_json, nodes_json) {
        cJSON *id_json = cJSON_GetObjectItem(node_json, "id");
        cJSON *x_json = cJSON_GetObjectItem(node_json, "x");
        cJSON *y_json = cJSON_GetObjectItem(node_json, "y");

        if (!cJSON_IsString(id_json) || !cJSON_IsNumber(x_json) || !cJSON_IsNumber(y_json))
            return false;

        const char *id = id_json->valuestring;
        float x = (float)x_json->valuedouble;
        float y = (float)y_json->valuedouble;

        if (node_array_add(nodes, id, addr++, x, y) < 0)
            return false;
    }

    return true;
}

static bool load_radio(cJSON *radio_json, radio_config_t *radio) {
    radio_config_init(radio);

    if (!radio_json)
        return true;  /* Use defaults */

    cJSON *range = cJSON_GetObjectItem(radio_json, "range");
    cJSON *loss = cJSON_GetObjectItem(radio_json, "loss_pct");
    cJSON *speed = cJSON_GetObjectItem(radio_json, "propagation_speed_ms_per_unit");

    if (range && cJSON_IsNumber(range))
        radio->range = (float)range->valuedouble;
    if (loss && cJSON_IsNumber(loss))
        radio->loss_pct = (float)loss->valuedouble;
    if (speed && cJSON_IsNumber(speed))
        radio->propagation_speed_ms_per_unit = (float)speed->valuedouble;

    return true;
}

static bool load_events(cJSON *events_json, event_queue_t *queue, node_array_t *nodes, radio_config_t *radio) {
    (void)radio;  /* Reserved for future use */
    if (!events_json)
        return true;  /* No scripted events */

    if (!cJSON_IsArray(events_json))
        return false;

    cJSON *evt_json = NULL;
    cJSON_ArrayForEach(evt_json, events_json) {
        cJSON *at_ms_json = cJSON_GetObjectItem(evt_json, "at_ms");
        cJSON *type_json = cJSON_GetObjectItem(evt_json, "type");

        if (!cJSON_IsNumber(at_ms_json) || !cJSON_IsString(type_json))
            return false;

        uint64_t timestamp_us = (uint64_t)(at_ms_json->valuedouble * 1000.0);
        const char *type = type_json->valuestring;

        sim_event_t event = {0};
        event.timestamp_us = timestamp_us;

        if (strcmp(type, "send_message") == 0 || strcmp(type, "generate_message") == 0) {
            event.type = EVT_GENERATE_MESSAGE;
            cJSON *src = cJSON_GetObjectItem(evt_json, "src");
            cJSON *dest = cJSON_GetObjectItem(evt_json, "dest");
            if (!cJSON_IsString(src) || !cJSON_IsString(dest))
                return false;
            
            sim_node_t *src_node = node_array_find_by_id(nodes, src->valuestring);
            sim_node_t *dest_node = node_array_find_by_id(nodes, dest->valuestring);
            if (!src_node || !dest_node)
                return false;
            
            strncpy(event.data.node.node_id, src->valuestring, NODE_ID_LEN - 1);
            event.data.node.addr = dest_node->addr;

        } else if (strcmp(type, "move_node") == 0) {
            event.type = EVT_NODE_MOVE;
            cJSON *node_id = cJSON_GetObjectItem(evt_json, "node");
            cJSON *x_json = cJSON_GetObjectItem(evt_json, "x");
            cJSON *y_json = cJSON_GetObjectItem(evt_json, "y");
            if (!cJSON_IsString(node_id) || !cJSON_IsNumber(x_json) || !cJSON_IsNumber(y_json))
                return false;
            
            strncpy(event.data.node.node_id, node_id->valuestring, NODE_ID_LEN - 1);
            event.data.node.x = (float)x_json->valuedouble;
            event.data.node.y = (float)y_json->valuedouble;

        } else if (strcmp(type, "kill_node") == 0 || strcmp(type, "node_leave") == 0) {
            event.type = EVT_NODE_LEAVE;
            cJSON *node_id = cJSON_GetObjectItem(evt_json, "node");
            if (!cJSON_IsString(node_id))
                return false;
            strncpy(event.data.node.node_id, node_id->valuestring, NODE_ID_LEN - 1);

        } else if (strcmp(type, "interference") == 0) {
            cJSON *x_json = cJSON_GetObjectItem(evt_json, "x");
            cJSON *y_json = cJSON_GetObjectItem(evt_json, "y");
            cJSON *radius_json = cJSON_GetObjectItem(evt_json, "radius");
            cJSON *duration_ms = cJSON_GetObjectItem(evt_json, "duration_ms");
            if (!cJSON_IsNumber(x_json) || !cJSON_IsNumber(y_json) || !cJSON_IsNumber(radius_json))
                return false;

            float x = (float)x_json->valuedouble;
            float y = (float)y_json->valuedouble;
            float radius = (float)radius_json->valuedouble;
            uint64_t duration = duration_ms ? (uint64_t)(duration_ms->valuedouble * 1000.0) : 0;

            /* Start interference */
            sim_event_t start_evt = {0};
            start_evt.timestamp_us = timestamp_us;
            start_evt.type = EVT_INTERFERENCE_START;
            start_evt.data.interference.center_x = x;
            start_evt.data.interference.center_y = y;
            start_evt.data.interference.radius = radius;
            event_queue_push(queue, &start_evt);

            /* End interference */
            if (duration > 0) {
                sim_event_t end_evt = {0};
                end_evt.timestamp_us = timestamp_us + duration;
                end_evt.type = EVT_INTERFERENCE_END;
                end_evt.data.interference.zone_index = -1;  /* Will be set when zone is created */
                event_queue_push(queue, &end_evt);
            }
            continue;  /* Don't push 'event' below */

        } else if (strcmp(type, "join") == 0 || strcmp(type, "node_join") == 0) {
            event.type = EVT_NODE_JOIN;
            cJSON *node_id = cJSON_GetObjectItem(evt_json, "node");
            if (!cJSON_IsString(node_id))
                return false;
            strncpy(event.data.node.node_id, node_id->valuestring, NODE_ID_LEN - 1);

        } else {
            fprintf(stderr, "Warning: unknown event type '%s' at %llu ms\n", type, 
                    (unsigned long long)(timestamp_us / 1000));
            continue;
        }

        event_queue_push(queue, &event);
    }

    return true;
}

bool scenario_load_file(const char *path, scenario_t *scenario) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open scenario file '%s'\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(size + 1);
    if (!data) {
        fclose(f);
        return false;
    }
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(data);
    free(data);

    if (!root) {
        fprintf(stderr, "Error: JSON parse failed\n");
        return false;
    }

    /* Load metadata */
    cJSON *name_json = cJSON_GetObjectItem(root, "name");
    cJSON *mode_json = cJSON_GetObjectItem(root, "mode");
    cJSON *duration_ms = cJSON_GetObjectItem(root, "duration_ms");
    cJSON *seed_json = cJSON_GetObjectItem(root, "seed");

    if (name_json && cJSON_IsString(name_json))
        strncpy(scenario->metadata.name, name_json->valuestring, sizeof(scenario->metadata.name) - 1);
    else
        strncpy(scenario->metadata.name, "unnamed", sizeof(scenario->metadata.name) - 1);

    scenario->metadata.deterministic = true;
    if (mode_json && cJSON_IsString(mode_json)) {
        if (strcmp(mode_json->valuestring, "stochastic") == 0) {
            fprintf(stderr, "Warning: stochastic mode not yet implemented, using deterministic\n");
            scenario->metadata.deterministic = false;
        }
    }

    scenario->metadata.duration_us = duration_ms && cJSON_IsNumber(duration_ms) ? 
        (uint64_t)(duration_ms->valuedouble * 1000.0) : 10000000;

    scenario->metadata.seed = seed_json && cJSON_IsNumber(seed_json) ? 
        (uint64_t)seed_json->valuedouble : 42;

    /* Load nodes */
    cJSON *nodes_json = cJSON_GetObjectItem(root, "nodes");
    if (!load_nodes(nodes_json, scenario->nodes)) {
        cJSON_Delete(root);
        return false;
    }

    /* Load radio config */
    cJSON *radio_json = cJSON_GetObjectItem(root, "radio");
    if (!load_radio(radio_json, scenario->radio)) {
        cJSON_Delete(root);
        return false;
    }

    /* Load scripted events */
    cJSON *events_json = cJSON_GetObjectItem(root, "events");
    if (!load_events(events_json, scenario->events, scenario->nodes, scenario->radio)) {
        cJSON_Delete(root);
        return false;
    }

    /* Schedule periodic metrics ticks every 5 seconds */
    for (uint64_t t = 5000000; t < scenario->metadata.duration_us; t += 5000000) {
        sim_event_t tick = {0};
        tick.timestamp_us = t;
        tick.type = EVT_METRICS_TICK;
        event_queue_push(scenario->events, &tick);
    }

    cJSON_Delete(root);
    return true;
}
