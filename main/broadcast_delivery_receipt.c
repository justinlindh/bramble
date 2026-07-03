#include "broadcast_delivery_receipt.h"

#include "packet.h"
#include "routing_auth.h"

/* Receipt slot timing — controls how delivery receipts from multiple nodes
 * spread out in time after receiving the same broadcast.  Wider spacing
 * reduces collision probability when many nodes try to TX receipts
 * simultaneously.  At SF10/125kHz a receipt packet takes ~150-200ms
 * airtime, so slots need to be wider than that. */
#define BROADCAST_RECEIPT_DELAY_BASE_MS      300u
#define BROADCAST_RECEIPT_SLOT_SPACING_MS    500u
#define BROADCAST_RECEIPT_SLOT_BUCKETS       32u
#define BROADCAST_RECEIPT_RETRY_COUNT        3u

#define RECEIPT_POLICY_FULL_MAX_PEERS       15u
#define RECEIPT_POLICY_NEIGHBORS_MAX_PEERS  40u
#define RECEIPT_BUDGET_MAX_MS               12000u

uint8_t mesh_broadcast_receipt_policy(uint32_t dest_addr, uint8_t peer_count) {
    if (dest_addr != 0xFFFFFFFFu) return 0;
    if (peer_count > RECEIPT_POLICY_NEIGHBORS_MAX_PEERS) return 0;  /* off */
    if (peer_count > RECEIPT_POLICY_FULL_MAX_PEERS) return 1;       /* neighbors-only */
    return 2;                                                        /* full */
}

bool mesh_should_emit_broadcast_delivery_receipt(uint32_t dest_addr, uint8_t peer_count) {
    return mesh_broadcast_receipt_policy(dest_addr, peer_count) > 0;
}

uint32_t mesh_broadcast_receipt_slot_delay_ms(uint32_t local_addr, uint32_t original_packet_id) {
    uint32_t slot = (local_addr ^ original_packet_id) % BROADCAST_RECEIPT_SLOT_BUCKETS;
    return BROADCAST_RECEIPT_DELAY_BASE_MS + (slot * BROADCAST_RECEIPT_SLOT_SPACING_MS);
}

uint8_t mesh_broadcast_receipt_retry_count(void) {
    return BROADCAST_RECEIPT_RETRY_COUNT;
}

void mesh_broadcast_receipt_retry_scale(uint32_t receipt_budget_remaining_ms,
                                        uint32_t *scale_num,
                                        uint32_t *scale_den) {
    if (!scale_num || !scale_den) {
        return;
    }

    uint32_t remaining = receipt_budget_remaining_ms;
    if (remaining > RECEIPT_BUDGET_MAX_MS) {
        remaining = RECEIPT_BUDGET_MAX_MS;
    }

    uint32_t used = RECEIPT_BUDGET_MAX_MS - remaining;
    uint32_t pct_used = (used * 100u) / RECEIPT_BUDGET_MAX_MS;

    if (pct_used < 30u) {
        *scale_num = 1u;
        *scale_den = 2u;
    } else if (pct_used > 70u) {
        *scale_num = 2u;
        *scale_den = 1u;
    } else {
        *scale_num = 1u;
        *scale_den = 1u;
    }
}

uint32_t mesh_broadcast_receipt_scale_delay_ms(uint32_t raw_delay_ms,
                                               uint32_t receipt_budget_remaining_ms) {
    uint32_t scale_num = 1u;
    uint32_t scale_den = 1u;

    mesh_broadcast_receipt_retry_scale(receipt_budget_remaining_ms, &scale_num, &scale_den);
    return (raw_delay_ms * scale_num) / scale_den;
}

esp_err_t mesh_build_broadcast_delivery_receipt_packet(uint32_t local_addr,
                                                       uint32_t receipt_packet_id,
                                                       uint32_t original_src_addr,
                                                       uint32_t original_packet_id,
                                                       uint8_t hop_limit,
                                                       uint64_t seq,
                                                       uint8_t *buf,
                                                       size_t buf_len,
                                                       size_t *out_len) {
    if (!buf || !out_len) {
        return ESP_FAIL;
    }

    bramble_delivery_receipt_t receipt = {
        .header = {
            .version = BRAMBLE_VERSION,
            .type = PKT_TYPE_DELIVERY_RECEIPT,
            .flags = 0,
            .hop_limit = hop_limit,
            .dest_addr = original_src_addr,
            .packet_id = receipt_packet_id,
        },
        .src_addr = local_addr,
        .orig_packet_id = original_packet_id,
        .hop_count = 1,
        .total_latency = 0,
        .relay_path = { local_addr },
        .seq = {
            (uint8_t)(seq >> 40), (uint8_t)(seq >> 32), (uint8_t)(seq >> 24),
            (uint8_t)(seq >> 16), (uint8_t)(seq >> 8), (uint8_t)seq,
        },
    };
    /* NEW-SEC-8 (STAGED): sign after every field except relay_path/
     * hop_count/hop_limit is set (excluded from the MAC, legitimately
     * change per relay hop); seq is set above and IS covered (ws 1.3b). */
    receipt_sign(&receipt);

    esp_err_t err = bramble_delivery_receipt_serialize(&receipt, buf, buf_len);
    if (err != ESP_OK) {
        return err;
    }

    *out_len = DELIVERY_RECEIPT_MIN_SIZE + ((size_t)receipt.hop_count * 4u);
    return ESP_OK;
}
