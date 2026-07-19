#ifndef SIM_PROBE_H
#define SIM_PROBE_H

#include <stdint.h>
#include "../components/bramble_probe/include/bramble_probe.h"

#define SIM_PROBE_MAX_NODES 256

void sim_probe_init(void);
void sim_probe_init_node(int node_id, uint32_t addr);
void sim_probe_tick(int node_id, uint32_t now_ms);

/* Packet dispatch: call from radio/bridge layer */
void sim_probe_handle_packet(int node_id, const uint8_t* data, uint16_t len, int8_t rssi,
                             uint32_t now_ms);

/* Access state for a node */
bramble_probe_state_t* sim_probe_get_state(int node_id);

/* Initiate a probe from a node */
int sim_probe_send(int node_id, uint8_t flags, uint32_t now_ms);

#endif /* SIM_PROBE_H */
