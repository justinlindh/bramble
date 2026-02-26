/**
 * @file mesh_msg_state.c
 * @brief Message queue and reliability state management implementation
 *
 * Stub implementation — actual state migration from mesh_task.c will
 * happen in a follow-up refactoring session.
 */

#include "mesh_msg_state.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mesh_msg_state";

/* ── State ──────────────────────────────────────────────────────────── */

static mesh_queued_msg_t   s_queued_msgs[MESH_MAX_QUEUED_MSGS];
static pending_ack_table_t s_pending_acks;
static reassembly_ctx_t    s_reassembly;

/* ── API Implementation ─────────────────────────────────────────────── */

void mesh_msg_state_init(void) {
    memset(s_queued_msgs, 0, sizeof(s_queued_msgs));
    pending_ack_init(&s_pending_acks);
    reassembly_init(&s_reassembly);
}

/* ── Queued messages ────────────────────────────────────────────────── */

int mesh_msg_queue(uint32_t dest_addr, const uint8_t *data, size_t len, uint32_t now_ms) {
    if (!data || len == 0 || len > BRAMBLE_MAX_PACKET_SIZE) {
        return -1;
    }

    for (int i = 0; i < MESH_MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used) {
            s_queued_msgs[i].dest_addr = dest_addr;
            memcpy(s_queued_msgs[i].data, data, len);
            s_queued_msgs[i].len = len;
            s_queued_msgs[i].timestamp = now_ms;
            s_queued_msgs[i].used = true;
            ESP_LOGI(TAG, "Queued msg for %08lX (slot %d)", (unsigned long)dest_addr, i);
            return 0;
        }
    }
    ESP_LOGW(TAG, "Message queue full, dropping msg for %08lX", (unsigned long)dest_addr);
    return -1;
}

void mesh_msg_flush_for(uint32_t dest_addr, mesh_msg_flush_cb_t callback, void *ctx) {
    for (int i = 0; i < MESH_MAX_QUEUED_MSGS; i++) {
        if (s_queued_msgs[i].used && s_queued_msgs[i].dest_addr == dest_addr) {
            if (callback) {
                callback(s_queued_msgs[i].data, s_queued_msgs[i].len, dest_addr, ctx);
            }
            s_queued_msgs[i].used = false;
        }
    }
}

void mesh_msg_expire(uint32_t now_ms, uint32_t max_age_ms) {
    for (int i = 0; i < MESH_MAX_QUEUED_MSGS; i++) {
        if (s_queued_msgs[i].used && (now_ms - s_queued_msgs[i].timestamp) > max_age_ms) {
            ESP_LOGW(TAG, "Queued msg for %08lX expired", (unsigned long)s_queued_msgs[i].dest_addr);
            s_queued_msgs[i].used = false;
        }
    }
}

bool mesh_msg_has_queued(uint32_t dest_addr) {
    for (int i = 0; i < MESH_MAX_QUEUED_MSGS; i++) {
        if (s_queued_msgs[i].used && s_queued_msgs[i].dest_addr == dest_addr) {
            return true;
        }
    }
    return false;
}

/* ── Pending ACKs ───────────────────────────────────────────────────── */

pending_ack_table_t *mesh_msg_state_get_pending_acks(void) {
    return &s_pending_acks;
}

int mesh_pending_ack_add(uint32_t packet_id, uint32_t dest_addr, uint8_t tier,
                         const uint8_t *packet_data, uint16_t packet_len, uint32_t now_ms) {
    return pending_ack_add(&s_pending_acks, packet_id, dest_addr, tier, packet_data, packet_len, now_ms);
}

bool mesh_pending_ack_remove(uint32_t packet_id) {
    return pending_ack_remove(&s_pending_acks, packet_id);
}

pending_ack_t *mesh_pending_ack_lookup(uint32_t packet_id) {
    /* Iterate to find by packet_id - pending_ack_table_t doesn't have a lookup by ID function */
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (s_pending_acks.entries[i].active && s_pending_acks.entries[i].packet_id == packet_id) {
            return &s_pending_acks.entries[i];
        }
    }
    return NULL;
}

/* ── Reassembly ─────────────────────────────────────────────────────── */

reassembly_ctx_t *mesh_msg_state_get_reassembly(void) {
    return &s_reassembly;
}

int mesh_reassembly_add(const frag_header_t *hdr, const uint8_t *payload,
                        size_t payload_len, uint32_t now_ms) {
    return reassembly_add(&s_reassembly, hdr, payload, payload_len, now_ms);
}

int mesh_reassembly_collect(uint16_t message_id, uint8_t *out, size_t out_max) {
    return reassembly_collect(&s_reassembly, message_id, out, out_max);
}

void mesh_reassembly_purge(uint32_t now_ms) {
    reassembly_purge(&s_reassembly, now_ms);
}
