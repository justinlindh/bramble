/**
 * @file mesh_dedup_state.h
 * @brief Packet deduplication state management for mesh task
 *
 * This module encapsulates the packet deduplication buffer that was
 * previously a static global (s_dedup) in mesh_task.c.
 *
 * Part of the mesh_task.c refactoring effort.
 */

#ifndef BRAMBLE_MESH_DEDUP_STATE_H
#define BRAMBLE_MESH_DEDUP_STATE_H

#include "dedup.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the dedup state module.
 * Must be called once at startup before any other dedup state APIs.
 */
void mesh_dedup_state_init(void);

/**
 * Get a pointer to the dedup buffer.
 * The returned pointer is valid for the lifetime of the application.
 *
 * @return Pointer to the dedup buffer
 */
dedup_buffer_t *mesh_dedup_state_get(void);

/**
 * Check if a packet has been seen recently, and add it if not.
 * This is the primary deduplication check for incoming packets.
 *
 * @param packet_id Packet identifier (may include type/source in key)
 * @param now_ms Current timestamp in milliseconds
 * @return true if this is a duplicate (already seen), false if new
 */
bool mesh_dedup_check(uint32_t packet_id, uint32_t now_ms);

/**
 * Purge expired entries from the dedup buffer.
 * Should be called periodically (e.g., every 60 seconds).
 *
 * @param now_ms Current timestamp in milliseconds
 */
void mesh_dedup_purge(uint32_t now_ms);

/**
 * Get the current number of entries in the dedup buffer.
 *
 * @return Number of entries
 */
int mesh_dedup_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_MESH_DEDUP_STATE_H */
