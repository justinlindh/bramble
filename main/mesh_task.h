#ifndef BRAMBLE_MESH_TASK_H
#define BRAMBLE_MESH_TASK_H

#include "identity.h"
#include "routing.h"
#include "dedup.h"
#include "freq_plan.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "public_channel.h"
#include "airtime_budget.h"

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
    airtime_budget_t airtime;
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
 * Returns packet_id (>0) on success, 0 on failure.
 */
uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len);

/**
 * Schedule a system reboot after delay_ms milliseconds.
 * Uses a one-shot FreeRTOS timer; safe to call from any task.
 */
void mesh_reboot_delayed(int delay_ms);

/**
 * Get a snapshot of the routing table (thread-safe).
 */
void mesh_get_routes(routing_table_t *out);

/**
 * Add a channel to the mesh. Returns channel index or -1 on error.
 */
int mesh_add_channel(const char *name, const uint8_t *psk, size_t psk_len);

/**
 * Remove a channel by index. Returns 0 on success.
 * Cannot remove index 0 (public channel).
 */
int mesh_remove_channel(int index);

/**
 * Get channel count.
 */
int mesh_get_channel_count(void);

/**
 * Set default channel (swap to index 0). Returns 0 on success.
 */
int mesh_set_default_channel(int index);

/**
 * Send a network reachability probe (broadcast).
 * Returns the probe packet ID.
 */
uint32_t mesh_send_probe(void);

/**
 * Enable/disable mailbox (store-and-forward for offline neighbors).
 */
void mesh_set_node_name(const char *name);
void mesh_set_mailbox(bool enabled);
bool mesh_get_mailbox(void);

#endif
