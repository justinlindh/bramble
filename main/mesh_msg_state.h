/**
 * @file mesh_msg_state.h
 * @brief Message queue and reliability state management for mesh task
 *
 * This module encapsulates message-related state that was previously
 * static globals in mesh_task.c:
 *   - Queued messages awaiting route discovery (s_queued_msgs)
 *   - Pending ACK table for reliability (s_pending_acks)
 *   - Fragment reassembly context (s_reassembly)
 *
 * Part of the mesh_task.c refactoring effort.
 */

#ifndef BRAMBLE_MESH_MSG_STATE_H
#define BRAMBLE_MESH_MSG_STATE_H

#include "reliability.h"
#include "fragment.h"
#include "packet.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of messages that can be queued awaiting route discovery */
#define MESH_MAX_QUEUED_MSGS 8

/**
 * Queued message entry — stores message data while waiting for route discovery.
 */
typedef struct {
    uint32_t dest_addr;                         /**< Destination address */
    uint8_t  data[BRAMBLE_MAX_PACKET_SIZE];     /**< Message data */
    size_t   len;                               /**< Message length */
    uint32_t timestamp;                         /**< Queue timestamp (ms) */
    bool     used;                              /**< Entry in use flag */
} mesh_queued_msg_t;

/**
 * Initialize the message state module.
 * Must be called once at startup before any other message state APIs.
 */
void mesh_msg_state_init(void);

/* ── Queued message APIs ──────────────────────────────────────────────── */

/**
 * Queue a message awaiting route discovery.
 *
 * @param dest_addr Destination address
 * @param data Message data
 * @param len Message length
 * @param now_ms Current timestamp
 * @return 0 on success, -1 if queue is full
 */
int mesh_msg_queue(uint32_t dest_addr, const uint8_t *data, size_t len, uint32_t now_ms);

/**
 * Flush (send) all queued messages for a destination that now has a route.
 * Caller is responsible for actually transmitting the messages.
 *
 * @param dest_addr Destination address
 * @param callback Function to call for each queued message (data, len, dest_addr)
 * @param ctx User context passed to callback
 */
typedef void (*mesh_msg_flush_cb_t)(const uint8_t *data, size_t len, uint32_t dest_addr, void *ctx);
void mesh_msg_flush_for(uint32_t dest_addr, mesh_msg_flush_cb_t callback, void *ctx);

/**
 * Expire old queued messages.
 *
 * @param now_ms Current timestamp
 * @param max_age_ms Maximum age in milliseconds before expiry
 */
void mesh_msg_expire(uint32_t now_ms, uint32_t max_age_ms);

/**
 * Check if there are any queued messages for a destination.
 *
 * @param dest_addr Destination address
 * @return true if messages are queued
 */
bool mesh_msg_has_queued(uint32_t dest_addr);

/* ── Pending ACK APIs ─────────────────────────────────────────────────── */

/**
 * Get a pointer to the pending ACK table.
 *
 * @return Pointer to pending ACK table
 */
pending_ack_table_t *mesh_msg_state_get_pending_acks(void);

/**
 * Add a packet to the pending ACK table for reliability tracking.
 *
 * @param packet_id Packet identifier
 * @param dest_addr Destination address
 * @param tier Message tier (affects retry timing)
 * @param packet_data Raw packet data for retransmission
 * @param packet_len Packet length
 * @param now_ms Current timestamp
 * @return 0 on success, -1 if table is full
 */
int mesh_pending_ack_add(uint32_t packet_id, uint32_t dest_addr, uint8_t tier,
                         const uint8_t *packet_data, uint16_t packet_len, uint32_t now_ms);

/**
 * Remove a packet from pending ACK table (ACK received).
 *
 * @param packet_id Packet identifier
 * @return true if entry was found and removed
 */
bool mesh_pending_ack_remove(uint32_t packet_id);

/**
 * Lookup a pending ACK entry.
 *
 * @param packet_id Packet identifier
 * @return Pointer to entry, or NULL if not found
 */
pending_ack_t *mesh_pending_ack_lookup(uint32_t packet_id);

/* ── Reassembly APIs ──────────────────────────────────────────────────── */

/**
 * Get a pointer to the reassembly context.
 *
 * @return Pointer to reassembly context
 */
reassembly_ctx_t *mesh_msg_state_get_reassembly(void);

/**
 * Add a fragment to the reassembly buffer.
 *
 * @param hdr Fragment header
 * @param payload Fragment payload (excluding header)
 * @param payload_len Payload length
 * @param now_ms Current timestamp
 * @return 1 if reassembly complete, 0 if more fragments needed, -1 on error
 */
int mesh_reassembly_add(const frag_header_t *hdr, const uint8_t *payload,
                        size_t payload_len, uint32_t now_ms);

/**
 * Collect a fully reassembled message.
 *
 * @param message_id Message identifier
 * @param out Output buffer for reassembled message
 * @param out_max Maximum output buffer size
 * @return Reassembled message length, or -1 on error
 */
int mesh_reassembly_collect(uint16_t message_id, uint8_t *out, size_t out_max);

/**
 * Purge expired reassembly entries.
 *
 * @param now_ms Current timestamp
 */
void mesh_reassembly_purge(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_MESH_MSG_STATE_H */
