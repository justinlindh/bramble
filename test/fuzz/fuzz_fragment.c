/* libFuzzer harness for the Bramble fragment reassembler in
 * components/fragment/fragment.c.
 *
 * Reassembly runs on decrypted payload bytes, but the fragment header
 * (index, total, message id) is carried inside that payload and is chosen
 * by whoever holds the channel key, so an insider can drive this state
 * machine with arbitrary values. That is the threat model this harness
 * covers: a long, adversarially interleaved command stream against a
 * single reassembly context.
 *
 * Input encoding: a sequence of 7-byte commands, each optionally followed
 * by payload bytes.
 *
 *   [0] op          selects add / collect / purge / first_packet_id / split
 *   [1] frag_index
 *   [2] frag_total
 *   [3] message_id low byte
 *   [4] message_id high byte
 *   [5] payload length requested by the command
 *   [6] milliseconds to advance the clock before the command runs
 *
 * Payload buffers are heap allocations sized exactly to the length the
 * command asks for, so an over-read past a fragment's real extent lands in
 * an ASan redzone instead of in unrelated stack.
 *
 * Build and run: bash test/fuzz/run_fuzz.sh
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fragment.h"

#define CMD_SIZE 7
#define REASSEMBLED_MAX (FRAG_MAX_FRAGMENTS * FRAG_MAX_PLAINTEXT)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    uint32_t now_ms = 0;
    size_t pos = 0;

    while (size - pos >= CMD_SIZE) {
        const uint8_t* cmd = data + pos;
        pos += CMD_SIZE;

        frag_header_t hdr = {
            .frag_index = cmd[1],
            .frag_total = cmd[2],
            .message_id = (uint16_t)((uint16_t)cmd[4] << 8 | cmd[3]),
        };

        /* Clock only ever moves forward, the way a real node's tick does.
         * A 255ms maximum step keeps a short input from trivially jumping
         * the whole 30s reassembly timeout, so the interesting mid-window
         * interleavings stay reachable. */
        now_ms += cmd[6];

        /* The fragment payload never exceeds what the input actually
         * supplies, so the allocation below always holds real bytes and
         * the redzone starts exactly where the fragment ends. */
        size_t want = cmd[5];
        size_t avail = size - pos;
        size_t frag_len = want < avail ? want : avail;

        uint8_t* frag = (uint8_t*)malloc(frag_len ? frag_len : 1);
        if (!frag)
            return 0;
        memcpy(frag, data + pos, frag_len);
        pos += frag_len;

        switch (cmd[0] % 5) {
        case 0: {
            uint32_t packet_id = (uint32_t)hdr.message_id << 16 | cmd[1];
            int r = reassembly_add(&ctx, &hdr, frag, frag_len, now_ms, packet_id);
            assert(r == -1 || r == 0 || r == 1);
            break;
        }
        case 1: {
            uint8_t* out = (uint8_t*)malloc(REASSEMBLED_MAX);
            if (out) {
                int r = reassembly_collect(&ctx, hdr.message_id, out, REASSEMBLED_MAX);
                /* A completed message can never exceed the maximum
                 * fragment count times the maximum fragment payload;
                 * anything larger means the slot bookkeeping let a
                 * write run past the caller's buffer. */
                assert(r <= REASSEMBLED_MAX);
                free(out);
            }
            break;
        }
        case 2:
            reassembly_purge(&ctx, now_ms);
            break;
        case 3:
            (void)reassembly_get_first_packet_id(&ctx, hdr.message_id);
            break;
        default: {
            /* fragment_split is the transmit side, but it shares the
             * FRAG_MAX_* bounds with the reassembler and a mismatch
             * between the two is exactly the kind of defect a shared
             * corpus surfaces. */
            fragment_t frags[FRAG_MAX_FRAGMENTS];
            int n = fragment_split(frag, frag_len, hdr.message_id, frags,
                                   (int)(cmd[2] % (FRAG_MAX_FRAGMENTS + 1)));
            assert(n <= FRAG_MAX_FRAGMENTS);
            for (int i = 0; i < n; i++)
                assert(frags[i].len <= FRAG_HEADER_SIZE + FRAG_MAX_PLAINTEXT);
            break;
        }
        }

        free(frag);
    }

    return 0;
}
