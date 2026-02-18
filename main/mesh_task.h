#ifndef BRAMBLE_MESH_TASK_H
#define BRAMBLE_MESH_TASK_H

#include "identity.h"
#include "routing.h"
#include "dedup.h"
#include "freq_plan.h"

/* Shared mesh state — protected by mutex, read by UI task */
typedef struct {
    neighbor_table_t neighbors;
    uint32_t         beacon_tx_count;
    uint32_t         beacon_rx_count;
    uint32_t         packets_tx;
    uint32_t         packets_rx;
    bool             radio_ok;
    int16_t          last_rx_rssi;
    int8_t           last_rx_snr;
} mesh_shared_state_t;

/**
 * Start the mesh task on CPU1.
 * Initializes radio, begins beacon TX/RX.
 */
void mesh_task_start(bramble_identity_t *identity);

/**
 * Get a snapshot of shared state (thread-safe).
 */
void mesh_get_state(mesh_shared_state_t *out);

#endif
