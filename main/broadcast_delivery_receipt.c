#include "broadcast_delivery_receipt.h"

#include "packet.h"

#define BROADCAST_RECEIPT_DELAY_BASE_MS      180u
#define BROADCAST_RECEIPT_SLOT_SPACING_MS    140u
#define BROADCAST_RECEIPT_SLOT_BUCKETS       16u
#define BROADCAST_RECEIPT_RETRY_COUNT        3u

bool mesh_should_emit_broadcast_delivery_receipt(uint32_t dest_addr) {
    return dest_addr == 0xFFFFFFFFu;
}

uint32_t mesh_broadcast_receipt_slot_delay_ms(uint32_t local_addr, uint32_t original_packet_id) {
    uint32_t slot = (local_addr ^ original_packet_id) % BROADCAST_RECEIPT_SLOT_BUCKETS;
    return BROADCAST_RECEIPT_DELAY_BASE_MS + (slot * BROADCAST_RECEIPT_SLOT_SPACING_MS);
}

uint8_t mesh_broadcast_receipt_retry_count(void) {
    return BROADCAST_RECEIPT_RETRY_COUNT;
}

esp_err_t mesh_build_broadcast_delivery_receipt_packet(uint32_t local_addr,
                                                       uint32_t receipt_packet_id,
                                                       uint32_t original_src_addr,
                                                       uint32_t original_packet_id,
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
            .hop_limit = 8,
            .dest_addr = original_src_addr,
            .packet_id = receipt_packet_id,
        },
        .src_addr = local_addr,
        .orig_packet_id = original_packet_id,
        .hop_count = 1,
        .total_latency = 0,
        .relay_path = { local_addr },
    };

    esp_err_t err = bramble_delivery_receipt_serialize(&receipt, buf, buf_len);
    if (err != ESP_OK) {
        return err;
    }

    *out_len = DELIVERY_RECEIPT_MIN_SIZE + ((size_t)receipt.hop_count * 4u);
    return ESP_OK;
}
