/**
 * @file mesh_dedup_state.c
 * @brief Packet deduplication state management implementation
 *
 * Stub implementation — actual state migration from mesh_task.c will
 * happen in a follow-up refactoring session.
 */

#include "mesh_dedup_state.h"

/* ── State ──────────────────────────────────────────────────────────── */

static dedup_buffer_t s_dedup;

/* ── API Implementation ─────────────────────────────────────────────── */

void mesh_dedup_state_init(void) {
    dedup_init(&s_dedup);
}

dedup_buffer_t *mesh_dedup_state_get(void) {
    return &s_dedup;
}

bool mesh_dedup_check(uint32_t packet_id, uint32_t now_ms) {
    return dedup_check_and_add(&s_dedup, packet_id, now_ms);
}

void mesh_dedup_purge(uint32_t now_ms) {
    dedup_purge(&s_dedup, now_ms);
}

int mesh_dedup_count(void) {
    return dedup_count(&s_dedup);
}
