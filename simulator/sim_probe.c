#include "sim_probe.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    bramble_probe_state_t probe_state;
    bool initialized;
} sim_probe_node_t;

static sim_probe_node_t probe_nodes[SIM_PROBE_MAX_NODES];

/* Captured packets for sim delivery */
typedef struct {
    uint8_t data[256];
    uint16_t len;
    int node_id;
} sim_probe_outbound_t;

static sim_probe_outbound_t last_outbound;

static void sim_probe_send_cb(const uint8_t* data, uint16_t len, void* ctx) {
    int node_id = (int)(intptr_t)ctx;
    if (len > 256)
        len = 256;
    memcpy(last_outbound.data, data, len);
    last_outbound.len = len;
    last_outbound.node_id = node_id;

    uint8_t type = (len >= 2) ? data[1] : 0;
    if (type == BRAMBLE_TYPE_BROADCAST_PROBE) {
        printf("[sim_probe] node %d sent probe (len=%u)\n", node_id, len);
    } else if (type == BRAMBLE_TYPE_BROADCAST_ACK) {
        printf("[sim_probe] node %d sent ACK (len=%u)\n", node_id, len);
    }
}

void sim_probe_init(void) { memset(probe_nodes, 0, sizeof(probe_nodes)); }

void sim_probe_init_node(int node_id, uint32_t addr) {
    if (node_id < 0 || node_id >= SIM_PROBE_MAX_NODES)
        return;
    sim_probe_node_t* n = &probe_nodes[node_id];
    bramble_probe_init(&n->probe_state, addr, sim_probe_send_cb, (void*)(intptr_t)node_id);
    n->initialized = true;
}

void sim_probe_tick(int node_id, uint32_t now_ms) {
    if (node_id < 0 || node_id >= SIM_PROBE_MAX_NODES)
        return;
    sim_probe_node_t* n = &probe_nodes[node_id];
    if (!n->initialized)
        return;

    bool was_collecting = n->probe_state.collecting;
    bramble_probe_tick(&n->probe_state, now_ms);

    /* Emit event on collection complete */
    if (was_collecting && !n->probe_state.collecting && n->probe_state.result.complete) {
        printf("[sim_probe] node %d collection complete: %u responses for probe 0x%08x\n", node_id,
               n->probe_state.result.response_count, n->probe_state.result.probe_id);
    }
}

void sim_probe_handle_packet(int node_id, const uint8_t* data, uint16_t len, int8_t rssi,
                             uint32_t now_ms) {
    if (node_id < 0 || node_id >= SIM_PROBE_MAX_NODES)
        return;
    sim_probe_node_t* n = &probe_nodes[node_id];
    if (!n->initialized || len < 2)
        return;

    uint8_t type = data[1];
    if (type == BRAMBLE_TYPE_BROADCAST_PROBE) {
        bramble_probe_handle_probe(&n->probe_state, data, len, rssi, now_ms);
    } else if (type == BRAMBLE_TYPE_BROADCAST_ACK) {
        bramble_probe_handle_ack(&n->probe_state, data, len, now_ms);
    }
}

bramble_probe_state_t* sim_probe_get_state(int node_id) {
    if (node_id < 0 || node_id >= SIM_PROBE_MAX_NODES)
        return NULL;
    if (!probe_nodes[node_id].initialized)
        return NULL;
    return &probe_nodes[node_id].probe_state;
}

int sim_probe_send(int node_id, uint8_t flags, uint32_t now_ms) {
    if (node_id < 0 || node_id >= SIM_PROBE_MAX_NODES)
        return -1;
    sim_probe_node_t* n = &probe_nodes[node_id];
    if (!n->initialized)
        return -1;
    return bramble_probe_send(&n->probe_state, flags, now_ms);
}
