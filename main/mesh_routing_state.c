/**
 * @file mesh_routing_state.c
 * @brief Routing state management implementation
 *
 * Stub implementation — actual state migration from mesh_task.c will
 * happen in a follow-up refactoring session.
 */

#include "mesh_routing_state.h"
#include <string.h>

/* ── State ──────────────────────────────────────────────────────────── */

static routing_table_t            s_routes;
static rreq_dedup_t               s_rreq_dedup;
static reverse_route_table_t      s_reverse_routes;
static pending_discovery_table_t  s_pending_disc;
static rreq_rate_limiter_t        s_rreq_rl;

/* ── API Implementation ─────────────────────────────────────────────── */

void mesh_routing_state_init(void) {
    route_init(&s_routes);
    rreq_dedup_init(&s_rreq_dedup);
    reverse_route_init(&s_reverse_routes);
    discovery_init(&s_pending_disc);
    rreq_rate_init(&s_rreq_rl);
}

/* ── Route table ────────────────────────────────────────────────────── */

routing_table_t *mesh_routing_state_get_routes(void) {
    return &s_routes;
}

route_entry_t *mesh_route_lookup(uint32_t dest_addr) {
    return route_lookup(&s_routes, dest_addr);
}

void mesh_route_install(uint32_t dest, uint32_t next_hop, uint8_t hop_count,
                        uint8_t metric, route_state_t state, uint32_t now_ms) {
    route_install(&s_routes, dest, next_hop, hop_count, metric, state, now_ms);
}

void mesh_route_maintenance(uint32_t now_ms) {
    route_maintenance(&s_routes, now_ms);
}

void mesh_route_snapshot(routing_table_t *out) {
    if (out) {
        memcpy(out, &s_routes, sizeof(routing_table_t));
    }
}

/* ── RREQ deduplication ─────────────────────────────────────────────── */

bool mesh_rreq_dedup_check_and_add(uint32_t query_id, uint32_t now_ms) {
    return rreq_dedup_check_and_add(&s_rreq_dedup, query_id, now_ms);
}

/* ── Reverse routes ─────────────────────────────────────────────────── */

reverse_route_table_t *mesh_routing_state_get_reverse_routes(void) {
    return &s_reverse_routes;
}

int mesh_reverse_route_add(uint32_t query_id, uint32_t prev_hop, uint32_t now_ms) {
    return reverse_route_add(&s_reverse_routes, query_id, prev_hop, now_ms);
}

reverse_route_t *mesh_reverse_route_lookup(uint32_t query_id) {
    return reverse_route_lookup(&s_reverse_routes, query_id);
}

void mesh_reverse_route_purge(uint32_t now_ms) {
    reverse_route_purge(&s_reverse_routes, now_ms);
}

/* ── Discovery ──────────────────────────────────────────────────────── */

pending_discovery_table_t *mesh_routing_state_get_pending_disc(void) {
    return &s_pending_disc;
}

int mesh_discovery_start(uint32_t dest_addr, uint32_t query_id, uint32_t now_ms) {
    return discovery_start(&s_pending_disc, dest_addr, query_id, now_ms);
}

pending_discovery_t *mesh_discovery_lookup(uint32_t dest_addr) {
    return discovery_lookup(&s_pending_disc, dest_addr);
}

pending_discovery_t *mesh_discovery_lookup_by_query(uint32_t query_id) {
    return discovery_lookup_by_query(&s_pending_disc, query_id);
}

void mesh_discovery_remove(uint32_t dest_addr) {
    discovery_remove(&s_pending_disc, dest_addr);
}

bool mesh_discovery_should_retry(const pending_discovery_t *d, uint32_t now_ms) {
    return discovery_should_retry(d, now_ms);
}

void mesh_discovery_record_attempt(pending_discovery_t *d, uint32_t query_id, uint32_t now_ms) {
    discovery_record_attempt(d, query_id, now_ms);
}

/* ── Rate limiter ───────────────────────────────────────────────────── */

rreq_rate_limiter_t *mesh_routing_state_get_rate_limiter(void) {
    return &s_rreq_rl;
}

bool mesh_rreq_rate_allow(uint32_t src_addr, uint32_t dest_addr, uint32_t now_ms) {
    return rreq_rate_allow(&s_rreq_rl, src_addr, dest_addr, now_ms);
}
