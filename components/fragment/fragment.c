#include "include/fragment.h"
#include <string.h>

int fragment_split(const uint8_t* plaintext, size_t pt_len, uint16_t message_id,
                   fragment_t* frags_out, int max_frags) {
    if (pt_len == 0 || !plaintext || !frags_out)
        return -1;

    int num_frags = (int)((pt_len + FRAG_MAX_PLAINTEXT - 1) / FRAG_MAX_PLAINTEXT);
    if (num_frags > FRAG_MAX_FRAGMENTS || num_frags > max_frags)
        return -1;

    for (int i = 0; i < num_frags; i++) {
        size_t offset = (size_t)i * FRAG_MAX_PLAINTEXT;
        size_t chunk = pt_len - offset;
        if (chunk > FRAG_MAX_PLAINTEXT)
            chunk = FRAG_MAX_PLAINTEXT;

        frags_out[i].data[0] = (uint8_t)i;
        frags_out[i].data[1] = (uint8_t)num_frags;
        frags_out[i].data[2] = (uint8_t)(message_id & 0xFF);
        frags_out[i].data[3] = (uint8_t)((message_id >> 8) & 0xFF);
        memcpy(&frags_out[i].data[FRAG_HEADER_SIZE], plaintext + offset, chunk);
        frags_out[i].len = FRAG_HEADER_SIZE + chunk;
    }
    return num_frags;
}

void reassembly_init(reassembly_ctx_t* ctx) { memset(ctx, 0, sizeof(*ctx)); }

int reassembly_add(reassembly_ctx_t* ctx, const frag_header_t* hdr, const uint8_t* frag_data,
                   size_t frag_len, uint32_t now_ms, uint32_t packet_id) {
    if (!ctx || !hdr || !frag_data)
        return -1;
    if (hdr->frag_index >= hdr->frag_total)
        return -1;
    if (hdr->frag_total > FRAG_MAX_FRAGMENTS)
        return -1;
    if (frag_len > FRAG_MAX_PLAINTEXT)
        return -1;

    /* Find existing slot or allocate new one */
    reassembly_slot_t* slot = NULL;
    for (int i = 0; i < FRAG_MAX_REASSEMBLIES; i++) {
        if (ctx->slots[i].active && ctx->slots[i].message_id == hdr->message_id) {
            /* Check timeout */
            if (now_ms - ctx->slots[i].start_time > FRAG_REASSEMBLY_TIMEOUT_MS) {
                ctx->slots[i].active = false;
                return -1;
            }
            slot = &ctx->slots[i];
            break;
        }
    }

    if (!slot) {
        for (int i = 0; i < FRAG_MAX_REASSEMBLIES; i++) {
            if (!ctx->slots[i].active) {
                slot = &ctx->slots[i];
                slot->active = true;
                slot->message_id = hdr->message_id;
                slot->total = hdr->frag_total;
                slot->received_mask = 0;
                slot->start_time = now_ms;
                break;
            }
        }
    }

    if (!slot)
        return -1;

    /* Track the packet_id of the first fragment received */
    if (slot->received_mask == 0) {
        slot->first_packet_id = packet_id;
    }

    /* Duplicate check */
    uint8_t bit = (uint8_t)(1 << hdr->frag_index);
    if (slot->received_mask & bit)
        return 0;

    memcpy(slot->data[hdr->frag_index], frag_data, frag_len);
    slot->frag_lens[hdr->frag_index] = frag_len;
    slot->received_mask |= bit;

    /* Check completeness */
    uint8_t full_mask = (uint8_t)((1 << slot->total) - 1);
    if (slot->received_mask == full_mask)
        return 1;
    return 0;
}

int reassembly_collect(reassembly_ctx_t* ctx, uint16_t message_id, uint8_t* out, size_t out_max) {
    for (int i = 0; i < FRAG_MAX_REASSEMBLIES; i++) {
        if (ctx->slots[i].active && ctx->slots[i].message_id == message_id) {
            reassembly_slot_t* slot = &ctx->slots[i];
            size_t total_len = 0;
            for (int j = 0; j < slot->total; j++) {
                total_len += slot->frag_lens[j];
            }
            if (total_len > out_max)
                return -1;

            size_t offset = 0;
            for (int j = 0; j < slot->total; j++) {
                memcpy(out + offset, slot->data[j], slot->frag_lens[j]);
                offset += slot->frag_lens[j];
            }
            slot->active = false;
            return (int)total_len;
        }
    }
    return -1;
}

uint32_t reassembly_get_first_packet_id(reassembly_ctx_t* ctx, uint16_t message_id) {
    for (int i = 0; i < FRAG_MAX_REASSEMBLIES; i++) {
        if (ctx->slots[i].active && ctx->slots[i].message_id == message_id) {
            return ctx->slots[i].first_packet_id;
        }
    }
    return 0;
}

void reassembly_purge(reassembly_ctx_t* ctx, uint32_t now_ms) {
    for (int i = 0; i < FRAG_MAX_REASSEMBLIES; i++) {
        if (ctx->slots[i].active &&
            now_ms - ctx->slots[i].start_time > FRAG_REASSEMBLY_TIMEOUT_MS) {
            ctx->slots[i].active = false;
        }
    }
}
