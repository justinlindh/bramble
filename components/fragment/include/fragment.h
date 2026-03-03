#ifndef BRAMBLE_FRAGMENT_H
#define BRAMBLE_FRAGMENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FRAG_MAX_PLAINTEXT       154
#define FRAG_MAX_FRAGMENTS       4
#define FRAG_MAX_REASSEMBLIES    4
#define FRAG_REASSEMBLY_TIMEOUT_MS 30000
#define FRAG_HEADER_SIZE         4

/* reassembly_slot_t.received_mask is uint8_t; keep fragment count <= bit width. */
_Static_assert(FRAG_MAX_FRAGMENTS <= 8, "received_mask overflow");

typedef struct {
    uint8_t  frag_index;
    uint8_t  frag_total;
    uint16_t message_id;
} frag_header_t;

typedef struct {
    uint8_t data[FRAG_HEADER_SIZE + FRAG_MAX_PLAINTEXT];
    size_t  len;
} fragment_t;

int fragment_split(const uint8_t *plaintext, size_t pt_len, uint16_t message_id,
                   fragment_t *frags_out, int max_frags);

typedef struct {
    uint16_t message_id;
    uint8_t  total;
    uint8_t  received_mask;
    uint8_t  data[FRAG_MAX_FRAGMENTS][FRAG_MAX_PLAINTEXT];
    size_t   frag_lens[FRAG_MAX_FRAGMENTS];
    uint32_t start_time;
    bool     active;
} reassembly_slot_t;

typedef struct {
    reassembly_slot_t slots[FRAG_MAX_REASSEMBLIES];
} reassembly_ctx_t;

void reassembly_init(reassembly_ctx_t *ctx);
int  reassembly_add(reassembly_ctx_t *ctx, const frag_header_t *hdr,
                    const uint8_t *frag_data, size_t frag_len, uint32_t now_ms);
int  reassembly_collect(reassembly_ctx_t *ctx, uint16_t message_id,
                        uint8_t *out, size_t out_max);
void reassembly_purge(reassembly_ctx_t *ctx, uint32_t now_ms);

#endif
