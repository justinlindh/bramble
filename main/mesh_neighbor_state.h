/**
 * @file mesh_neighbor_state.h
 * @brief Neighbor table state management for mesh task
 *
 * This module encapsulates the neighbor table state that was previously
 * a static global in mesh_task.c. It provides accessor APIs for neighbor
 * table operations while hiding the internal storage.
 *
 * Part of the mesh_task.c refactoring effort.
 */

#ifndef BRAMBLE_MESH_NEIGHBOR_STATE_H
#define BRAMBLE_MESH_NEIGHBOR_STATE_H

#include "routing.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the neighbor state module.
 * Must be called once at startup before any other neighbor state APIs.
 */
void mesh_neighbor_state_init(void);

/**
 * Get a pointer to the neighbor table.
 * The returned pointer is valid for the lifetime of the application.
 * Caller must hold the state mutex when modifying.
 *
 * @return Pointer to the neighbor table
 */
neighbor_table_t *mesh_neighbor_state_get(void);

/**
 * Update a neighbor entry from a received beacon.
 *
 * @param addr Neighbor address
 * @param rssi RSSI of received beacon
 * @param snr SNR of received beacon
 * @param pubkey_hash Public key hash from beacon
 * @param now_ms Current timestamp in milliseconds
 * @return Index of updated/added entry, or -1 on error
 */
int mesh_neighbor_update(uint32_t addr, int8_t rssi, int8_t snr,
                         uint32_t pubkey_hash, uint32_t now_ms);

/**
 * Lookup a neighbor by address.
 *
 * @param addr Neighbor address to find
 * @return Pointer to neighbor entry, or NULL if not found
 */
neighbor_entry_t *mesh_neighbor_lookup(uint32_t addr);

/**
 * Purge expired neighbors from the table.
 *
 * @param now_ms Current timestamp in milliseconds
 */
void mesh_neighbor_purge(uint32_t now_ms);

/**
 * Get the current neighbor count.
 *
 * @return Number of active neighbors
 */
int mesh_neighbor_count(void);

/**
 * Copy the neighbor table to the output buffer (thread-safe snapshot).
 *
 * @param out Output buffer to receive the neighbor table copy
 */
void mesh_neighbor_snapshot(neighbor_table_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_MESH_NEIGHBOR_STATE_H */
