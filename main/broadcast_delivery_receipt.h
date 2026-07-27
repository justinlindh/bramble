#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Resolved by include path per build; see packet.h. */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Receipt policy based on mesh size:
 *   peer_count ≤ 15:  full receipts (multi-hop)
 *   peer_count ≤ 40:  neighbors-only (hop_limit=1)
 *   peer_count > 40:  receipts disabled for broadcast tier
 *
 * Returns: 0 = don't send, 1 = neighbors-only (hop_limit=1), 2 = full
 */
uint8_t mesh_broadcast_receipt_policy(uint32_t dest_addr, uint8_t peer_count);

/* Convenience: returns true if any receipt should be sent */
bool mesh_should_emit_broadcast_delivery_receipt(uint32_t dest_addr, uint8_t peer_count);

/* Deterministic responder slot base (ms) to spread receipt TX among recipients. */
/* peer_count: this node's current neighbor count; sizes the anti-collision
 * slot window (2 slots per peer, clamped to [4, 32] buckets). */
uint32_t mesh_broadcast_receipt_slot_delay_ms(uint32_t local_addr, uint32_t original_packet_id,
                                              uint8_t peer_count);

/* Number of on-air attempts for a broadcast delivery receipt. */
uint8_t mesh_broadcast_receipt_retry_count(void);

/*
 * Adaptive retry timing scale derived from receipt airtime budget utilization.
 *
 * Scale mapping (by % budget used):
 *   <30%   => 0.5x (num=1, den=2)
 *   30-70% => 1.0x (num=1, den=1)
 *   >70%   => 2.0x (num=2, den=1)
 */
void mesh_broadcast_receipt_retry_scale(uint32_t receipt_budget_remaining_ms, uint32_t* scale_num,
                                        uint32_t* scale_den);

/*
 * seq (ws 1.3b): the 48-bit control-plane origin sequence, drawn by the
 * CALLER via control_seq_next (mesh_beacon.c) and written into the struct
 * before receipt_sign. This function lives in a host-testable file (no
 * FreeRTOS/ESP-IDF dependency) and cannot reach control_seq_next itself,
 * same reason rrep_build_destination in components/routing/discovery.c
 * doesn't draw its own seq either.
 */
esp_err_t mesh_build_broadcast_delivery_receipt_packet(
    uint32_t local_addr, uint32_t receipt_packet_id, uint32_t original_src_addr,
    uint32_t original_packet_id, uint8_t hop_limit, uint64_t seq, uint8_t* buf, size_t buf_len,
    size_t* out_len);

#ifdef __cplusplus
}
#endif
