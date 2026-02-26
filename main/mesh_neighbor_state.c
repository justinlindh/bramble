/**
 * @file mesh_neighbor_state.c
 * @brief Neighbor table state management implementation
 *
 * Stub implementation — actual state migration from mesh_task.c will
 * happen in a follow-up refactoring session.
 */

#include "mesh_neighbor_state.h"
#include <string.h>

/* ── State ──────────────────────────────────────────────────────────── */

static neighbor_table_t s_neighbors;

/* ── API Implementation ─────────────────────────────────────────────── */

void mesh_neighbor_state_init(void) {
    neighbor_init(&s_neighbors);
}

neighbor_table_t *mesh_neighbor_state_get(void) {
    return &s_neighbors;
}

int mesh_neighbor_update(uint32_t addr, int8_t rssi, int8_t snr,
                         uint32_t pubkey_hash, uint32_t now_ms) {
    return neighbor_update(&s_neighbors, addr, rssi, snr, pubkey_hash, now_ms);
}

neighbor_entry_t *mesh_neighbor_lookup(uint32_t addr) {
    return neighbor_lookup(&s_neighbors, addr);
}

void mesh_neighbor_purge(uint32_t now_ms) {
    neighbor_purge(&s_neighbors, now_ms);
}

int mesh_neighbor_count(void) {
    return neighbor_count(&s_neighbors);
}

void mesh_neighbor_snapshot(neighbor_table_t *out) {
    if (out) {
        memcpy(out, &s_neighbors, sizeof(neighbor_table_t));
    }
}
