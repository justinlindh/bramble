#include "sim_scenario.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

/*
 * Sample an exponentially-distributed interval for a Poisson process.
 * rate_per_min: average events per minute (must be > 0).
 * Returns interval in microseconds.
 */
static uint64_t poisson_interval_us(pcg32_state_t* rng, float rate_per_min) {
    if (rate_per_min <= 0.0f)
        return UINT64_MAX;
    float rate_per_us = rate_per_min / 60000000.0f;
    float u = pcg32_float(rng);
    if (u < 1e-9f)
        u = 1e-9f; /* Guard against log(0) */
    float interval = -logf(u) / rate_per_us;
    return (uint64_t)interval;
}

/* ── Deterministic helpers (reused from original) ─────────────────────── */

static bool load_nodes(cJSON* nodes_json, node_array_t* nodes) {
    if (!cJSON_IsArray(nodes_json))
        return false;

    uint32_t addr = 0x01000000; /* Auto-assign addresses */
    cJSON* node_json = NULL;
    cJSON_ArrayForEach(node_json, nodes_json) {
        cJSON* id_json = cJSON_GetObjectItem(node_json, "id");
        cJSON* x_json = cJSON_GetObjectItem(node_json, "x");
        cJSON* y_json = cJSON_GetObjectItem(node_json, "y");

        if (!cJSON_IsString(id_json) || !cJSON_IsNumber(x_json) || !cJSON_IsNumber(y_json))
            return false;

        const char* id = id_json->valuestring;
        float x = (float)x_json->valuedouble;
        float y = (float)y_json->valuedouble;

        if (node_array_add(nodes, id, addr++, x, y) < 0)
            return false;
    }

    return true;
}

static bool load_radio(cJSON* radio_json, radio_config_t* radio) {
    radio_config_init(radio);

    if (!radio_json)
        return true; /* Use defaults */

    cJSON* range = cJSON_GetObjectItem(radio_json, "range");
    cJSON* loss = cJSON_GetObjectItem(radio_json, "loss_pct");
    cJSON* speed = cJSON_GetObjectItem(radio_json, "propagation_speed_ms_per_unit");

    if (loss && cJSON_IsNumber(loss))
        radio->loss_pct = (float)loss->valuedouble;
    if (speed && cJSON_IsNumber(speed))
        radio->propagation_speed_ms_per_unit = (float)speed->valuedouble;

    /* LoRa PHY + collision model overrides (defaults mirror the firmware's
     * long-range profile; see radio_config_init) */
    cJSON* sf = cJSON_GetObjectItem(radio_json, "sf");
    cJSON* bw = cJSON_GetObjectItem(radio_json, "bw_hz");
    cJSON* cr = cJSON_GetObjectItem(radio_json, "cr");
    cJSON* txp = cJSON_GetObjectItem(radio_json, "tx_power_dbm");
    cJSON* cap = cJSON_GetObjectItem(radio_json, "capture_db");
    cJSON* ple = cJSON_GetObjectItem(radio_json, "path_loss_exp");
    cJSON* col = cJSON_GetObjectItem(radio_json, "collisions");
    cJSON* lbt = cJSON_GetObjectItem(radio_json, "lbt");

    if (sf && cJSON_IsNumber(sf))
        radio->sf = (uint8_t)sf->valuedouble;
    if (bw && cJSON_IsNumber(bw))
        radio->bw_hz = (uint32_t)bw->valuedouble;
    if (cr && cJSON_IsNumber(cr))
        radio->cr = (uint8_t)cr->valuedouble;
    if (txp && cJSON_IsNumber(txp))
        radio->tx_power_dbm = (int8_t)txp->valuedouble;
    if (cap && cJSON_IsNumber(cap))
        radio->capture_db = (float)cap->valuedouble;
    if (ple && cJSON_IsNumber(ple))
        radio->path_loss_exp = (float)ple->valuedouble;
    if (col && cJSON_IsBool(col))
        radio->collisions_enabled = cJSON_IsTrue(col);
    if (lbt && cJSON_IsBool(lbt))
        radio->lbt_enabled = cJSON_IsTrue(lbt);

    /* Range: an explicit "range" always wins, preserving the disk-range
     * escape hatch topology tests rely on. Otherwise derive it from the link
     * budget implied by whatever sf/bw_hz/tx_power_dbm/path_loss_* this
     * scenario just set, so SF/BW changes couple to range instead of a fixed
     * disk independent of the radio's own settings (must run after the PHY
     * overrides above). */
    if (range && cJSON_IsNumber(range))
        radio->range = (float)range->valuedouble;
    else
        radio->range = radio_derive_range(radio);

    /* Optional regulatory duty-cycle cap (DES-8): absent = unlimited,
     * matching today's behavior (radio_config_init already left
     * duty_cycle_set false). When present, applied via the real
     * airtime_budget_set_duty_cap on every node (bridge_apply_duty_cycle_cap),
     * never computed here. */
    cJSON* duty = cJSON_GetObjectItem(radio_json, "duty_cycle_pct");
    if (duty && cJSON_IsNumber(duty)) {
        double pct = duty->valuedouble;
        if (pct < 0.0)
            pct = 0.0;
        if (pct > 100.0)
            pct = 100.0;
        radio->duty_cycle_set = true;
        radio->duty_cycle_pct = (uint8_t)pct;
    }

    return true;
}

/*
 * Beacon interval policy (sim_node.h sim_beacon_policy_t), one shared
 * instance for the whole scenario, consumed by beacon_interval_decide()
 * (main/beacon_policy_calc.c) every node_tick. Defaults mirror firmware's
 * shipped BEACON_MODE_FIXED config (mesh_task.c:298-306): adaptive
 * disabled, fixed 60s. adaptive=true opts into the same dense/churn policy
 * firmware's opt-in adaptive mode uses.
 */
static bool load_beacon_policy(cJSON* beacon_json, sim_beacon_policy_t* beacon) {
    sim_beacon_policy_init(beacon);

    if (!beacon_json || !cJSON_IsObject(beacon_json))
        return true; /* no override: firmware defaults stand */

    cJSON* adaptive = cJSON_GetObjectItem(beacon_json, "adaptive");
    cJSON* interval_ms = cJSON_GetObjectItem(beacon_json, "interval_ms");
    cJSON* min_ms = cJSON_GetObjectItem(beacon_json, "min_interval_ms");
    cJSON* max_ms = cJSON_GetObjectItem(beacon_json, "max_interval_ms");
    cJSON* dense_th = cJSON_GetObjectItem(beacon_json, "dense_threshold");
    cJSON* churn_th = cJSON_GetObjectItem(beacon_json, "churn_threshold");
    cJSON* churn_window_ms = cJSON_GetObjectItem(beacon_json, "churn_window_ms");

    if (adaptive && cJSON_IsBool(adaptive))
        beacon->adaptive = cJSON_IsTrue(adaptive);
    if (interval_ms && cJSON_IsNumber(interval_ms))
        beacon->interval_ms = (uint32_t)interval_ms->valuedouble;
    if (min_ms && cJSON_IsNumber(min_ms))
        beacon->min_interval_ms = (uint32_t)min_ms->valuedouble;
    if (max_ms && cJSON_IsNumber(max_ms))
        beacon->max_interval_ms = (uint32_t)max_ms->valuedouble;
    if (dense_th && cJSON_IsNumber(dense_th))
        beacon->dense_threshold = (uint8_t)dense_th->valuedouble;
    if (churn_th && cJSON_IsNumber(churn_th))
        beacon->churn_threshold = (uint8_t)churn_th->valuedouble;
    if (churn_window_ms && cJSON_IsNumber(churn_window_ms))
        beacon->churn_window_ms = (uint32_t)churn_window_ms->valuedouble;

    return true;
}

static bool load_events(cJSON* events_json, event_queue_t* queue, node_array_t* nodes,
                        radio_config_t* radio) {
    (void)radio; /* Reserved for future use */
    if (!events_json)
        return true; /* No scripted events */

    if (!cJSON_IsArray(events_json))
        return false;

    cJSON* evt_json = NULL;
    cJSON_ArrayForEach(evt_json, events_json) {
        cJSON* at_ms_json = cJSON_GetObjectItem(evt_json, "at_ms");
        cJSON* type_json = cJSON_GetObjectItem(evt_json, "type");

        if (!cJSON_IsNumber(at_ms_json) || !cJSON_IsString(type_json))
            return false;

        uint64_t timestamp_us = (uint64_t)(at_ms_json->valuedouble * 1000.0);
        const char* type = type_json->valuestring;

        sim_event_t event = {0};
        event.timestamp_us = timestamp_us;

        if (strcmp(type, "send_message") == 0 || strcmp(type, "generate_message") == 0) {
            event.type = EVT_GENERATE_MESSAGE;
            cJSON* src = cJSON_GetObjectItem(evt_json, "src");
            cJSON* dest = cJSON_GetObjectItem(evt_json, "dest");
            if (!cJSON_IsString(src) || !cJSON_IsString(dest))
                return false;

            sim_node_t* src_node = node_array_find_by_id(nodes, src->valuestring);
            if (!src_node)
                return false;

            uint32_t dest_addr;
            if (strcmp(dest->valuestring, "*") == 0) {
                /* Broadcast/channel message (Task 5, channel flood): no
                 * single destination node to resolve. 0xFFFFFFFF mirrors
                 * firmware's mesh_send_broadcast/mesh_send_channel dest
                 * sentinel; bridge_handle_generate_message (gosim) branches
                 * on it the same way. */
                dest_addr = 0xFFFFFFFF;
            } else {
                sim_node_t* dest_node = node_array_find_by_id(nodes, dest->valuestring);
                if (!dest_node)
                    return false;
                dest_addr = dest_node->addr;
            }

            strncpy(event.data.node.node_id, src->valuestring, NODE_ID_LEN - 1);
            event.data.node.addr = dest_addr;

            /* payload_size stored in x field (Phase 4: fragmentation) */
            cJSON* ps = cJSON_GetObjectItem(evt_json, "payload_size");
            if (cJSON_IsNumber(ps)) {
                event.data.node.x = (float)ps->valuedouble;
            }

        } else if (strcmp(type, "send_attestation") == 0) {
            /* Per-node identity Phase 3/4: scripted identity-attestation
             * origination. "src" is the attesting node; optional "claim"
             * names the node whose ADDRESS is claimed (default: src
             * itself). claim != src is the impersonation scenario: a keyed
             * insider attesting someone else's address under its own keys
             * (post-Phase-4: refused by every receiver's addr<->key
             * check). Optional "rotate_x25519": true rotates the node's
             * X25519 pub before attesting (stored in the x field, like
             * generate_message's payload_size), the TOFU-conflict
             * scenario. */
            event.type = EVT_GENERATE_ATTESTATION;
            cJSON* src = cJSON_GetObjectItem(evt_json, "src");
            if (!cJSON_IsString(src))
                return false;
            sim_node_t* src_node = node_array_find_by_id(nodes, src->valuestring);
            if (!src_node)
                return false;
            strncpy(event.data.node.node_id, src->valuestring, NODE_ID_LEN - 1);
            event.data.node.addr = 0; /* 0 = claim own address */
            cJSON* claim = cJSON_GetObjectItem(evt_json, "claim");
            if (cJSON_IsString(claim)) {
                sim_node_t* claim_node = node_array_find_by_id(nodes, claim->valuestring);
                if (!claim_node)
                    return false;
                event.data.node.addr = claim_node->addr;
            }
            cJSON* rot = cJSON_GetObjectItem(evt_json, "rotate_x25519");
            if (cJSON_IsBool(rot) && cJSON_IsTrue(rot)) {
                event.data.node.x = 1.0f;
            }

        } else if (strcmp(type, "provision_anchor") == 0) {
            /* Trust-anchor campaign (P2 red-team): runtime anchor provisioning,
             * the sim analog of an operator running bramble.setAnchor mid-life.
             * "node" is (re-)anchored to the fleet test anchor; if it was
             * un-anchored ("unanchored": true at boot) its stale TOFU pins are
             * dropped by identity_store_set_anchor. */
            event.type = EVT_PROVISION_ANCHOR;
            cJSON* node_id = cJSON_GetObjectItem(evt_json, "node");
            if (!cJSON_IsString(node_id))
                return false;
            if (!node_array_find_by_id(nodes, node_id->valuestring))
                return false;
            strncpy(event.data.node.node_id, node_id->valuestring, NODE_ID_LEN - 1);

        } else if (strcmp(type, "move_node") == 0) {
            event.type = EVT_NODE_MOVE;
            cJSON* node_id = cJSON_GetObjectItem(evt_json, "node");
            cJSON* x_json = cJSON_GetObjectItem(evt_json, "x");
            cJSON* y_json = cJSON_GetObjectItem(evt_json, "y");
            if (!cJSON_IsString(node_id) || !cJSON_IsNumber(x_json) || !cJSON_IsNumber(y_json))
                return false;

            strncpy(event.data.node.node_id, node_id->valuestring, NODE_ID_LEN - 1);
            event.data.node.x = (float)x_json->valuedouble;
            event.data.node.y = (float)y_json->valuedouble;

        } else if (strcmp(type, "kill_node") == 0 || strcmp(type, "node_leave") == 0) {
            event.type = EVT_NODE_LEAVE;
            cJSON* node_id = cJSON_GetObjectItem(evt_json, "node");
            if (!cJSON_IsString(node_id))
                return false;
            strncpy(event.data.node.node_id, node_id->valuestring, NODE_ID_LEN - 1);

        } else if (strcmp(type, "interference") == 0) {
            cJSON* x_json = cJSON_GetObjectItem(evt_json, "x");
            cJSON* y_json = cJSON_GetObjectItem(evt_json, "y");
            cJSON* radius_json = cJSON_GetObjectItem(evt_json, "radius");
            cJSON* duration_ms = cJSON_GetObjectItem(evt_json, "duration_ms");
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
                end_evt.data.interference.zone_index = -1;
                event_queue_push(queue, &end_evt);
            }
            continue; /* Don't push 'event' below */

        } else if (strcmp(type, "join") == 0 || strcmp(type, "node_join") == 0) {
            event.type = EVT_NODE_JOIN;
            cJSON* node_id = cJSON_GetObjectItem(evt_json, "node");
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

/* ── Stochastic scenario loader ───────────────────────────────────────── */

static bool load_stochastic(cJSON* root, scenario_t* scenario) {
    /* Parse optional node block — may be {count, area} not an array */
    cJSON* nodes_cfg = cJSON_GetObjectItem(root, "nodes");
    cJSON* radio_json = cJSON_GetObjectItem(root, "radio");
    cJSON* chaos_cfg = cJSON_GetObjectItem(root, "chaos");
    cJSON* traffic_cfg = cJSON_GetObjectItem(root, "traffic");

    /* ── Node count & area ─────────────────────────────────────────────── */
    int node_count = 10;
    float area_w = 300.0f;
    float area_h = 300.0f;

    if (nodes_cfg) {
        if (cJSON_IsObject(nodes_cfg)) {
            cJSON* cnt = cJSON_GetObjectItem(nodes_cfg, "count");
            cJSON* area = cJSON_GetObjectItem(nodes_cfg, "area");
            if (cnt && cJSON_IsNumber(cnt))
                node_count = (int)cnt->valuedouble;
            if (area && cJSON_IsArray(area)) {
                cJSON* aw = cJSON_GetArrayItem(area, 0);
                cJSON* ah = cJSON_GetArrayItem(area, 1);
                if (aw && cJSON_IsNumber(aw))
                    area_w = (float)aw->valuedouble;
                if (ah && cJSON_IsNumber(ah))
                    area_h = (float)ah->valuedouble;
            }
        }
    }
    if (node_count > MAX_NODES)
        node_count = MAX_NODES;

    /* ── Radio config ──────────────────────────────────────────────────── */
    load_radio(radio_json, scenario->radio);
    if (radio_json && cJSON_IsObject(radio_json)) {
        cJSON* loss_range = cJSON_GetObjectItem(radio_json, "loss_pct_range");

        if (loss_range && cJSON_IsArray(loss_range)) {
            cJSON* lo = cJSON_GetArrayItem(loss_range, 0);
            cJSON* hi = cJSON_GetArrayItem(loss_range, 1);
            float lo_v = lo && cJSON_IsNumber(lo) ? (float)lo->valuedouble : 2.0f;
            float hi_v = hi && cJSON_IsNumber(hi) ? (float)hi->valuedouble : 15.0f;
            /* Use mid-range as fixed loss_pct; future work: per-link stochastic */
            scenario->radio->loss_pct = (lo_v + hi_v) / 2.0f;
        }
    }

    /* ── Seed the RNG ──────────────────────────────────────────────────── */
    pcg32_seed(scenario->rng, scenario->metadata.seed);

    /* ── Generate nodes at random positions ───────────────────────────── */
    uint32_t addr = 0x02000000; /* Different base from deterministic */
    for (int i = 0; i < node_count; i++) {
        char id[NODE_ID_LEN];
        snprintf(id, sizeof(id), "N%d", i);
        float x = pcg32_float(scenario->rng) * area_w;
        float y = pcg32_float(scenario->rng) * area_h;
        if (node_array_add(scenario->nodes, id, addr++, x, y) < 0) {
            fprintf(stderr, "Warning: node array full at %d nodes\n", i);
            break;
        }
    }

    uint64_t dur = scenario->metadata.duration_us;

    /* ── Chaos: node churn ─────────────────────────────────────────────── */
    float leave_rate = 0.0f;
    float join_rate = 0.0f;
    float speed_max = 0.0f;
    float ifreq = 0.0f;
    float iradius_min = 30.0f, iradius_max = 100.0f;
    float idur_min_us = 1000000.0f, idur_max_us = 5000000.0f;

    if (chaos_cfg && cJSON_IsObject(chaos_cfg)) {
        cJSON* churn = cJSON_GetObjectItem(chaos_cfg, "node_churn");
        cJSON* move = cJSON_GetObjectItem(chaos_cfg, "movement");
        cJSON* iface = cJSON_GetObjectItem(chaos_cfg, "interference");

        if (churn && cJSON_IsObject(churn)) {
            cJSON* lr = cJSON_GetObjectItem(churn, "leave_rate_per_min");
            cJSON* jr = cJSON_GetObjectItem(churn, "join_rate_per_min");
            if (lr && cJSON_IsNumber(lr))
                leave_rate = (float)lr->valuedouble;
            if (jr && cJSON_IsNumber(jr))
                join_rate = (float)jr->valuedouble;
        }
        if (move && cJSON_IsObject(move)) {
            cJSON* sm = cJSON_GetObjectItem(move, "speed_max");
            if (sm && cJSON_IsNumber(sm))
                speed_max = (float)sm->valuedouble;
        }
        if (iface && cJSON_IsObject(iface)) {
            cJSON* freq = cJSON_GetObjectItem(iface, "frequency_per_min");
            cJSON* rr = cJSON_GetObjectItem(iface, "radius_range");
            cJSON* dr = cJSON_GetObjectItem(iface, "duration_range_ms");
            if (freq && cJSON_IsNumber(freq))
                ifreq = (float)freq->valuedouble;
            if (rr && cJSON_IsArray(rr)) {
                cJSON* a = cJSON_GetArrayItem(rr, 0);
                cJSON* b = cJSON_GetArrayItem(rr, 1);
                if (a && cJSON_IsNumber(a))
                    iradius_min = (float)a->valuedouble;
                if (b && cJSON_IsNumber(b))
                    iradius_max = (float)b->valuedouble;
            }
            if (dr && cJSON_IsArray(dr)) {
                cJSON* a = cJSON_GetArrayItem(dr, 0);
                cJSON* b = cJSON_GetArrayItem(dr, 1);
                if (a && cJSON_IsNumber(a))
                    idur_min_us = (float)a->valuedouble * 1000.0f;
                if (b && cJSON_IsNumber(b))
                    idur_max_us = (float)b->valuedouble * 1000.0f;
            }
        }
    }

    /* Track which nodes have left (to allow rejoins) */
    bool node_left[MAX_NODES] = {false};

    /* Schedule leave events */
    if (leave_rate > 0.0f) {
        uint64_t t = poisson_interval_us(scenario->rng, leave_rate);
        while (t < dur) {
            /* Pick a random active node */
            int active_count = 0;
            int active_idx[MAX_NODES];
            for (int i = 0; i < scenario->nodes->count; i++) {
                if (!node_left[i])
                    active_idx[active_count++] = i;
            }
            if (active_count > 1) { /* Keep at least 1 active */
                int pick = (int)pcg32_range(scenario->rng, 0, (uint32_t)active_count);
                int idx = active_idx[pick];
                node_left[idx] = true;

                sim_event_t evt = {0};
                evt.timestamp_us = t;
                evt.type = EVT_NODE_LEAVE;
                snprintf(evt.data.node.node_id, NODE_ID_LEN, "%s", scenario->nodes->nodes[idx].id);
                event_queue_push(scenario->events, &evt);
            }
            t += poisson_interval_us(scenario->rng, leave_rate);
        }
    }

    /* Schedule join events for departed nodes */
    if (join_rate > 0.0f) {
        uint64_t t = poisson_interval_us(scenario->rng, join_rate);
        while (t < dur) {
            /* Pick a random departed node */
            int gone_count = 0;
            int gone_idx[MAX_NODES];
            for (int i = 0; i < scenario->nodes->count; i++) {
                if (node_left[i])
                    gone_idx[gone_count++] = i;
            }
            if (gone_count > 0) {
                int pick = (int)pcg32_range(scenario->rng, 0, (uint32_t)gone_count);
                int idx = gone_idx[pick];
                node_left[idx] = false;

                sim_event_t evt = {0};
                evt.timestamp_us = t;
                evt.type = EVT_NODE_JOIN;
                snprintf(evt.data.node.node_id, NODE_ID_LEN, "%s", scenario->nodes->nodes[idx].id);
                event_queue_push(scenario->events, &evt);
            }
            t += poisson_interval_us(scenario->rng, join_rate);
        }
    }

    /* ── Chaos: random walk movement ──────────────────────────────────── */
    if (speed_max > 0.0f) {
        /*
         * Each node moves every 5 seconds on average.
         * Displacement = speed_max * 5 s (a single-step random walk).
         */
        float move_rate = (float)scenario->nodes->count * 12.0f; /* ~12 moves/min per node */
        uint64_t t = poisson_interval_us(scenario->rng, move_rate);
        while (t < dur) {
            int idx = (int)pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count);
            sim_node_t* node = &scenario->nodes->nodes[idx];

            float dx = (pcg32_float(scenario->rng) * 2.0f - 1.0f) * speed_max * 5.0f;
            float dy = (pcg32_float(scenario->rng) * 2.0f - 1.0f) * speed_max * 5.0f;
            float nx = node->x + dx;
            float ny = node->y + dy;
            /* Clamp to area */
            if (nx < 0.0f)
                nx = 0.0f;
            if (ny < 0.0f)
                ny = 0.0f;
            if (nx > area_w)
                nx = area_w;
            if (ny > area_h)
                ny = area_h;

            sim_event_t evt = {0};
            evt.timestamp_us = t;
            evt.type = EVT_NODE_MOVE;
            snprintf(evt.data.node.node_id, NODE_ID_LEN, "%s", node->id);
            evt.data.node.x = nx;
            evt.data.node.y = ny;
            event_queue_push(scenario->events, &evt);

            /* Update node's stored position so subsequent moves are relative */
            node->x = nx;
            node->y = ny;

            t += poisson_interval_us(scenario->rng, move_rate);
        }
    }

    /* ── Chaos: interference zones ─────────────────────────────────────── */
    if (ifreq > 0.0f) {
        uint64_t t = poisson_interval_us(scenario->rng, ifreq);
        while (t < dur) {
            float cx = pcg32_float(scenario->rng) * area_w;
            float cy = pcg32_float(scenario->rng) * area_h;
            float r = iradius_min + pcg32_float(scenario->rng) * (iradius_max - iradius_min);
            float d = idur_min_us + pcg32_float(scenario->rng) * (idur_max_us - idur_min_us);

            sim_event_t start = {0};
            start.timestamp_us = t;
            start.type = EVT_INTERFERENCE_START;
            start.data.interference.center_x = cx;
            start.data.interference.center_y = cy;
            start.data.interference.radius = r;
            event_queue_push(scenario->events, &start);

            sim_event_t end = {0};
            end.timestamp_us = t + (uint64_t)d;
            end.type = EVT_INTERFERENCE_END;
            end.data.interference.zone_index = -1;
            event_queue_push(scenario->events, &end);

            t += poisson_interval_us(scenario->rng, ifreq);
        }
    }

    /* ── Traffic: random message pairs ────────────────────────────────── */
    float msg_rate = 5.0f;
    bool random_pairs = true;

    if (traffic_cfg && cJSON_IsObject(traffic_cfg)) {
        cJSON* mr = cJSON_GetObjectItem(traffic_cfg, "messages_per_min");
        cJSON* rp = cJSON_GetObjectItem(traffic_cfg, "random_pairs");
        if (mr && cJSON_IsNumber(mr))
            msg_rate = (float)mr->valuedouble;
        if (rp && cJSON_IsBool(rp))
            random_pairs = cJSON_IsTrue(rp);
    }

    if (msg_rate > 0.0f && scenario->nodes->count >= 2) {
        uint64_t t = poisson_interval_us(scenario->rng, msg_rate);
        while (t < dur) {
            int src_idx, dst_idx;
            if (random_pairs) {
                src_idx = (int)pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count);
                do {
                    dst_idx = (int)pcg32_range(scenario->rng, 0, (uint32_t)scenario->nodes->count);
                } while (dst_idx == src_idx);
            } else {
                src_idx = 0;
                dst_idx = 1;
            }

            sim_event_t evt = {0};
            evt.timestamp_us = t;
            evt.type = EVT_GENERATE_MESSAGE;
            snprintf(evt.data.node.node_id, NODE_ID_LEN, "%s", scenario->nodes->nodes[src_idx].id);
            evt.data.node.addr = scenario->nodes->nodes[dst_idx].addr;
            event_queue_push(scenario->events, &evt);

            t += poisson_interval_us(scenario->rng, msg_rate);
        }
    }

    return true;
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool scenario_load_file(const char* path, scenario_t* scenario) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open scenario file '%s'\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return false;
    }
    fread(data, 1, (size_t)size, f);
    data[size] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(data);
    free(data);

    if (!root) {
        fprintf(stderr, "Error: JSON parse failed\n");
        return false;
    }

    /* ── Metadata ──────────────────────────────────────────────────────── */
    cJSON* name_json = cJSON_GetObjectItem(root, "name");
    cJSON* mode_json = cJSON_GetObjectItem(root, "mode");
    cJSON* duration_ms = cJSON_GetObjectItem(root, "duration_ms");
    cJSON* seed_json = cJSON_GetObjectItem(root, "seed");

    if (name_json && cJSON_IsString(name_json))
        strncpy(scenario->metadata.name, name_json->valuestring,
                sizeof(scenario->metadata.name) - 1);
    else
        strncpy(scenario->metadata.name, "unnamed", sizeof(scenario->metadata.name) - 1);

    scenario->metadata.duration_us = duration_ms && cJSON_IsNumber(duration_ms)
                                         ? (uint64_t)(duration_ms->valuedouble * 1000.0)
                                         : 10000000ULL;

    scenario->metadata.seed =
        seed_json && cJSON_IsNumber(seed_json) ? (uint64_t)seed_json->valuedouble : 42ULL;

    bool stochastic = false;
    if (mode_json && cJSON_IsString(mode_json))
        stochastic = (strcmp(mode_json->valuestring, "stochastic") == 0);

    scenario->metadata.deterministic = !stochastic;

    /* ── Beacon policy (both modes, independent of node loading) ────────── */
    cJSON* beacon_json = cJSON_GetObjectItem(root, "beacon");
    load_beacon_policy(beacon_json, &scenario->beacon);

    /* ── Route to correct loader ───────────────────────────────────────── */
    bool ok;
    if (stochastic) {
        ok = load_stochastic(root, scenario);
    } else {
        /* Deterministic path */
        cJSON* nodes_json = cJSON_GetObjectItem(root, "nodes");
        ok = load_nodes(nodes_json, scenario->nodes);
        if (ok) {
            cJSON* radio_json = cJSON_GetObjectItem(root, "radio");
            ok = load_radio(radio_json, scenario->radio);
        }
        if (ok) {
            cJSON* events_json = cJSON_GetObjectItem(root, "events");
            ok = load_events(events_json, scenario->events, scenario->nodes, scenario->radio);
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        return false;
    }

    /* ── Periodic metrics ticks (both modes) ───────────────────────────── */
    for (uint64_t t = 5000000ULL; t < scenario->metadata.duration_us; t += 5000000ULL) {
        sim_event_t tick = {0};
        tick.timestamp_us = t;
        tick.type = EVT_METRICS_TICK;
        event_queue_push(scenario->events, &tick);
    }

    cJSON_Delete(root);
    return true;
}
