/**
 * @file mesh_routing_state.h
 * @brief Routing state management for mesh task
 *
 * This module encapsulates the routing-related state that was previously
 * static globals in mesh_task.c:
 *   - Route table (s_routes)
 *   - RREQ deduplication cache (s_rreq_dedup)
 *   - Reverse route table (s_reverse_routes)
 *   - Pending discovery table (s_pending_disc)
 *   - RREQ rate limiter (s_rreq_rl)
 *
 * Part of the mesh_task.c refactoring effort.
 */

#ifndef BRAMBLE_MESH_ROUTING_STATE_H
#define BRAMBLE_MESH_ROUTING_STATE_H

#include "routing.h"
#include "discovery.h"
#include "security.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize all routing state (route table, dedup, reverse routes, discovery, rate limiter).
 * Must be called once at startup before any other routing state APIs.
 */
void mesh_routing_state_init(void);

/* ── Route table APIs ─────────────────────────────────────────────────── */

/**
 * Get a pointer to the route table.
 *
 * @return Pointer to the routing table
 */
routing_table_t *mesh_routing_state_get_routes(void);

/**
 * Lookup a route to the given destination.
 *
 * @param dest_addr Destination address
 * @return Pointer to route entry, or NULL if not found
 */
route_entry_t *mesh_route_lookup(uint32_t dest_addr);

/**
 * Install or update a route.
 *
 * @param dest Destination address
 * @param next_hop Next hop address
 * @param hop_count Number of hops
 * @param metric Route metric
 * @param state Route state
 * @param now_ms Current timestamp
 */
void mesh_route_install(uint32_t dest, uint32_t next_hop, uint8_t hop_count,
                        uint8_t metric, route_state_t state, uint32_t now_ms);

/**
 * Perform route maintenance (purge stale routes, update states).
 *
 * @param now_ms Current timestamp
 */
void mesh_route_maintenance(uint32_t now_ms);

/**
 * Copy the route table to the output buffer (thread-safe snapshot).
 *
 * @param out Output buffer to receive the route table copy
 */
void mesh_route_snapshot(routing_table_t *out);

/* ── RREQ deduplication APIs ──────────────────────────────────────────── */

/**
 * Check if an RREQ query_id has been seen recently, and add it if not.
 *
 * @param query_id RREQ query identifier
 * @param now_ms Current timestamp
 * @return true if this is a duplicate (already seen), false if new
 */
bool mesh_rreq_dedup_check_and_add(uint32_t query_id, uint32_t now_ms);

/* ── Reverse route APIs ───────────────────────────────────────────────── */

/**
 * Get a pointer to the reverse route table.
 *
 * @return Pointer to reverse route table
 */
reverse_route_table_t *mesh_routing_state_get_reverse_routes(void);

/**
 * Add a reverse route entry for RREP path-back.
 *
 * @param query_id RREQ query identifier
 * @param prev_hop Previous hop address
 * @param now_ms Current timestamp
 * @return 0 on success, -1 on table full
 */
int mesh_reverse_route_add(uint32_t query_id, uint32_t prev_hop, uint32_t now_ms);

/**
 * Lookup a reverse route by query_id.
 *
 * @param query_id RREQ query identifier
 * @return Pointer to reverse route, or NULL if not found
 */
reverse_route_t *mesh_reverse_route_lookup(uint32_t query_id);

/**
 * Purge expired reverse routes.
 *
 * @param now_ms Current timestamp
 */
void mesh_reverse_route_purge(uint32_t now_ms);

/* ── Discovery APIs ───────────────────────────────────────────────────── */

/**
 * Get a pointer to the pending discovery table.
 *
 * @return Pointer to pending discovery table
 */
pending_discovery_table_t *mesh_routing_state_get_pending_disc(void);

/**
 * Start a new route discovery for the given destination.
 *
 * @param dest_addr Destination address
 * @param query_id Query identifier for this discovery
 * @param now_ms Current timestamp
 * @return 0 on success, -1 on table full
 */
int mesh_discovery_start(uint32_t dest_addr, uint32_t query_id, uint32_t now_ms);

/**
 * Lookup a pending discovery by destination address.
 *
 * @param dest_addr Destination address
 * @return Pointer to discovery entry, or NULL if not found
 */
pending_discovery_t *mesh_discovery_lookup(uint32_t dest_addr);

/**
 * Lookup a pending discovery by query_id.
 *
 * @param query_id Query identifier
 * @return Pointer to discovery entry, or NULL if not found
 */
pending_discovery_t *mesh_discovery_lookup_by_query(uint32_t query_id);

/**
 * Remove a pending discovery entry.
 *
 * @param dest_addr Destination address to remove
 */
void mesh_discovery_remove(uint32_t dest_addr);

/**
 * Check if a discovery should retry RREQ transmission.
 *
 * @param d Discovery entry
 * @param now_ms Current timestamp
 * @return true if retry is due
 */
bool mesh_discovery_should_retry(const pending_discovery_t *d, uint32_t now_ms);

/**
 * Record a discovery attempt.
 *
 * @param d Discovery entry
 * @param now_ms Current timestamp
 */
void mesh_discovery_record_attempt(pending_discovery_t *d, uint32_t now_ms);

/* ── RREQ rate limiter APIs ───────────────────────────────────────────── */

/**
 * Get a pointer to the RREQ rate limiter.
 *
 * @return Pointer to rate limiter
 */
rreq_rate_limiter_t *mesh_routing_state_get_rate_limiter(void);

/**
 * Check if an RREQ is allowed by the rate limiter.
 *
 * @param src_addr Source address initiating the RREQ
 * @param dest_addr Destination address being queried
 * @param now_ms Current timestamp
 * @return true if RREQ is allowed, false if rate limited
 */
bool mesh_rreq_rate_allow(uint32_t src_addr, uint32_t dest_addr, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_MESH_ROUTING_STATE_H */
