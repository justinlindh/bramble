#ifndef BRAMBLE_MESH_TASK_H
#define BRAMBLE_MESH_TASK_H

#include "identity.h"
#include "routing.h"
#include "dedup.h"
#include "freq_plan.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "public_channel.h"

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

/**
 * Send a broadcast message on the public channel.
 * Returns 0 on success.
 */
int mesh_send_broadcast(const uint8_t *data, size_t len);

/**
 * Send an encrypted message to a specific address.
 * Uses public channel for now (DM encryption requires key exchange).
 * Returns 0 on success.
 */
int mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len);

/**
 * Schedule a system reboot after delay_ms milliseconds.
 * Uses a one-shot FreeRTOS timer; safe to call from any task.
 */
void mesh_reboot_delayed(int delay_ms);

/**
 * Get a snapshot of the routing table (thread-safe).
 */
void mesh_get_routes(routing_table_t *out);

#endif
