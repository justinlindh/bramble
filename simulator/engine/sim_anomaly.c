#include "sim_anomaly.h"
#include "sim_emitter.h"
#include "sim_radio.h"
#include <string.h>
#include <stdio.h>

/* ── Init ─────────────────────────────────────────────────────────────── */

void anomaly_init(node_anomaly_tracker_t* t) { memset(t, 0, sizeof(*t)); }

/* ── Route flap ──────────────────────────────────────────────────────── */

bool anomaly_check_route_flap(route_flap_tracker_t* tracker, uint32_t dest_addr, uint32_t next_hop,
                              uint64_t now_us, FILE* emit_out, const char* node_id) {
    int flap_count = 0;
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->changes[i].dest_addr == dest_addr) {
            uint64_t age = now_us - tracker->changes[i].timestamp_us;
            if (age < ROUTE_FLAP_WINDOW_US)
                flap_count++;
        }
    }

    /* Add this change */
    int idx;
    if (tracker->count < MAX_ROUTE_FLAP_TRACK) {
        idx = tracker->count++;
    } else {
        idx = 0;
        for (int i = 1; i < MAX_ROUTE_FLAP_TRACK; i++) {
            if (tracker->changes[i].timestamp_us < tracker->changes[idx].timestamp_us)
                idx = i;
        }
    }
    tracker->changes[idx].dest_addr = dest_addr;
    tracker->changes[idx].next_hop = next_hop;
    tracker->changes[idx].timestamp_us = now_us;

    if (flap_count >= ROUTE_FLAP_THRESHOLD) {
        char details[128];
        snprintf(details, sizeof(details), "%d changes in 2s", flap_count);
        emit_anomaly(emit_out, now_us, "route_flap", node_id, dest_addr, details);
        return true;
    }

    return false;
}

/* ── Black hole ──────────────────────────────────────────────────────── */

void anomaly_record_rx(blackhole_tracker_t* t, uint64_t now_us) {
    /* Roll window if needed */
    if (now_us - t->window_start_us >= BLACKHOLE_WINDOW_US) {
        t->window_start_us = now_us;
        t->rx_count = 0;
        t->fwd_count = 0;
        t->reported = false;
    }
    t->rx_count++;
}

void anomaly_record_fwd(blackhole_tracker_t* t, uint64_t now_us) {
    if (now_us - t->window_start_us >= BLACKHOLE_WINDOW_US) {
        t->window_start_us = now_us;
        t->rx_count = 0;
        t->fwd_count = 0;
        t->reported = false;
    }
    t->fwd_count++;
}

bool anomaly_check_blackhole(blackhole_tracker_t* t, uint64_t now_us, FILE* emit_out,
                             const char* node_id) {
    if (t->reported)
        return false;
    if (now_us - t->window_start_us >= BLACKHOLE_WINDOW_US)
        return false;
    if (t->rx_count < BLACKHOLE_THRESHOLD)
        return false;
    if (t->fwd_count == 0) {
        char details[128];
        snprintf(details, sizeof(details), "received %u packets, forwarded 0 in 10s window",
                 t->rx_count);
        emit_anomaly(emit_out, now_us, "black_hole", node_id, 0, details);
        t->reported = true;
        return true;
    }
    return false;
}

/* ── Route loop ──────────────────────────────────────────────────────── */

bool anomaly_check_forward_loop(loop_tracker_t* t, uint32_t packet_id, uint8_t hop_limit,
                                uint64_t now_us, FILE* emit_out, const char* node_id) {
    /* Expire old entries */
    for (int i = 0; i < t->count; i++) {
        if (now_us - t->seen[i].first_seen_us > LOOP_TTL_US) {
            /* Evict: swap with last */
            t->seen[i] = t->seen[--t->count];
            i--;
        }
    }

    /* Checked at FORWARD time, with the received hop_limit as the
     * discriminator, because a packet_id a node merely SAW twice is normal
     * life on a mesh (flood rebroadcast, a sender's ACK retransmission, a
     * relay re-forwarding that retransmission). A retransmitted frame
     * retraces the same path and arrives at each relay with the SAME
     * hop_limit as before, while a packet trapped in a routing loop comes
     * back around with hop_limit lower by the loop length. Same id,
     * different hop_limit at the same relay = the packet transited this
     * node twice in one journey. */
    for (int i = 0; i < t->count; i++) {
        if (t->seen[i].packet_id == packet_id) {
            if (t->seen[i].hop_limit != hop_limit) {
                char details[128];
                snprintf(details, sizeof(details),
                         "packet 0x%08X transited this node twice (hop_limit %u then %u)",
                         packet_id, t->seen[i].hop_limit, hop_limit);
                emit_anomaly(emit_out, now_us, "route_loop", node_id, 0, details);
                return true;
            }
            return false; /* same hop_limit: a retransmission along the same path */
        }
    }

    /* Record this forward */
    if (t->count < MAX_LOOP_TRACK) {
        t->seen[t->count].packet_id = packet_id;
        t->seen[t->count].hop_limit = hop_limit;
        t->seen[t->count].first_seen_us = now_us;
        t->count++;
    } else {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < MAX_LOOP_TRACK; i++) {
            if (t->seen[i].first_seen_us < t->seen[oldest].first_seen_us)
                oldest = i;
        }
        t->seen[oldest].packet_id = packet_id;
        t->seen[oldest].hop_limit = hop_limit;
        t->seen[oldest].first_seen_us = now_us;
    }

    return false;
}

/* ── Excessive RREQ retransmission ───────────────────────────────────── */

bool anomaly_check_rreq_retx(rreq_retx_tracker_t* t, uint32_t dest_addr, uint64_t now_us,
                             FILE* emit_out, const char* node_id) {
    /* Find or create entry for this destination */
    rreq_retx_entry_t* entry = NULL;
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].dest_addr == dest_addr) {
            entry = &t->entries[i];
            break;
        }
    }
    if (!entry) {
        if (t->count >= MAX_RREQ_TRACK) {
            /* Evict first (simple LRU not needed; just reuse slot 0) */
            entry = &t->entries[0];
            entry->dest_addr = dest_addr;
            entry->count = 0;
        } else {
            entry = &t->entries[t->count++];
            entry->dest_addr = dest_addr;
            entry->count = 0;
        }
    }

    /* Expire timestamps outside window */
    int window_count = 0;
    int arr_size = (int)(sizeof(entry->timestamps) / sizeof(entry->timestamps[0]));
    for (int i = 0; i < entry->count && i < arr_size; i++) {
        if (now_us - entry->timestamps[i] <= RREQ_WINDOW_US) {
            entry->timestamps[window_count++] = entry->timestamps[i];
        }
    }
    entry->count = window_count;

    /* Add current timestamp */
    if (entry->count < arr_size) {
        entry->timestamps[entry->count++] = now_us;
    }

    if (entry->count >= RREQ_THRESHOLD) {
        char details[128];
        snprintf(details, sizeof(details), "%d RREQ retransmits for dest 0x%08X in 10s",
                 entry->count, dest_addr);
        emit_anomaly(emit_out, now_us, "excessive_rreq", node_id, dest_addr, details);
        /* Reset to avoid spamming */
        entry->count = 0;
        return true;
    }

    return false;
}

/* ── Mesh partition ──────────────────────────────────────────────────── */

int anomaly_partition_components(const node_array_t* nodes, const radio_config_t* radio,
                                 int* comp_out) {
    for (int i = 0; i < MAX_NODES; i++)
        comp_out[i] = -1;

    int n = nodes->count;
    int components = 0;
    int queue[MAX_NODES];

    for (int seed = 0; seed < n; seed++) {
        if (!nodes->nodes[seed].active || comp_out[seed] >= 0)
            continue;

        int head = 0, tail = 0;
        comp_out[seed] = components;
        queue[tail++] = seed;

        while (head < tail) {
            int cur = queue[head++];
            const sim_node_t* a = &nodes->nodes[cur];
            for (int j = 0; j < n; j++) {
                if (!nodes->nodes[j].active || comp_out[j] >= 0)
                    continue;
                if (radio_nodes_connected(radio, a, &nodes->nodes[j])) {
                    comp_out[j] = components;
                    queue[tail++] = j;
                }
            }
        }
        components++;
    }

    return components;
}

void anomaly_check_partition(node_array_t* nodes, const radio_config_t* radio, uint64_t now_us,
                             FILE* emit_out) {
    int n = nodes->count;
    if (n == 0)
        return;

    /* Collect active node indices */
    int active[MAX_NODES];
    int active_count = 0;
    for (int i = 0; i < n; i++) {
        if (nodes->nodes[i].active)
            active[active_count++] = i;
    }
    if (active_count <= 1)
        return; /* 0 or 1 nodes: nothing to partition */

    /* Anything outside the first active node's component is unreachable from
     * it, which is exactly what this detector has always reported. */
    int comp[MAX_NODES];
    anomaly_partition_components(nodes, radio, comp);
    int home = comp[active[0]];

    char unreachable_list[256] = "";
    int unreachable = 0;
    for (int j = 0; j < active_count; j++) {
        if (comp[active[j]] != home) {
            unreachable++;
            if (strlen(unreachable_list) + 4 < sizeof(unreachable_list)) {
                if (unreachable > 1)
                    strncat(unreachable_list, ",",
                            sizeof(unreachable_list) - strlen(unreachable_list) - 1);
                strncat(unreachable_list, nodes->nodes[active[j]].id,
                        sizeof(unreachable_list) - strlen(unreachable_list) - 1);
            }
        }
    }

    if (unreachable > 0) {
        char details[384];
        snprintf(details, sizeof(details), "mesh partitioned: %d nodes unreachable [%s]",
                 unreachable, unreachable_list);
        emit_anomaly(emit_out, now_us, "mesh_partition", "network", 0, details);
    }
}
