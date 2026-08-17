#include "nonce_counter.h"
#include <stdbool.h>

static uint32_t s_src;
static uint16_t s_salt;
static uint64_t s_counter;         /* next to issue; valid only while s_ready */
static uint64_t s_ceiling_durable; /* highest ceiling CONFIRMED by a successful write */
static nonce_store_read_fn s_rd;
static nonce_store_write_fn s_wr;
static void* s_ctx;
static bool s_ready; /* true once the initial reserve-ahead write is durable */

/* A write with no callback can never be confirmed durable, so it fails
 * closed exactly like a callback that returns an error. */
static int durable_write(uint64_t ceiling) {
    if (!s_wr)
        return -1;
    return s_wr(ceiling, s_ctx);
}

int nonce_counter_init(uint32_t src_addr, uint16_t boot_salt, nonce_store_read_fn rd,
                       nonce_store_write_fn wr, void* ctx) {
    s_src = src_addr;
    s_salt = boot_salt;
    s_rd = rd;
    s_wr = wr;
    s_ctx = ctx;
    s_ready = false;

    uint64_t ceiling = 0;
    if (s_rd)
        s_rd(&ceiling, s_ctx);

    uint64_t new_ceiling = ceiling + NONCE_RESERVE;
    if (durable_write(new_ceiling) != 0) {
        /* Reserve-ahead write not confirmed durable: the subsystem is
         * unusable (nonce_counter_next refuses to issue) until a later
         * init call succeeds. Do NOT advance s_counter/s_ceiling_durable
         * on a failed write; that would let issuance run ahead of what NVS
         * actually knows about. */
        return -1;
    }

    s_counter = ceiling;             /* resume above anything ever used */
    s_ceiling_durable = new_ceiling; /* reserve-ahead, now durable */
    s_ready = true;
    return 0;
}

int nonce_counter_next(uint8_t nonce_out[12]) {
    if (!s_ready)
        return -1;

    if (s_counter >= s_ceiling_durable) { /* flush-before-cross */
        uint64_t new_ceiling = s_ceiling_durable + NONCE_RESERVE;
        if (durable_write(new_ceiling) != 0) {
            /* Boundary flush not confirmed durable: issue NOTHING. Do not
             * increment s_counter here; the caller may retry once the write
             * path recovers and will get the same next counter value. */
            return -1;
        }
        s_ceiling_durable = new_ceiling;
    }

    uint64_t c = s_counter++; /* guaranteed c < s_ceiling_durable */
    nonce_out[0] = (uint8_t)(s_src >> 24);
    nonce_out[1] = (uint8_t)(s_src >> 16);
    nonce_out[2] = (uint8_t)(s_src >> 8);
    nonce_out[3] = (uint8_t)(s_src);
    nonce_out[4] = (uint8_t)(s_salt >> 8);
    nonce_out[5] = (uint8_t)(s_salt);
    nonce_out[6] = (uint8_t)(c >> 40);
    nonce_out[7] = (uint8_t)(c >> 32);
    nonce_out[8] = (uint8_t)(c >> 24);
    nonce_out[9] = (uint8_t)(c >> 16);
    nonce_out[10] = (uint8_t)(c >> 8);
    nonce_out[11] = (uint8_t)(c);
    return 0;
}

uint64_t nonce_counter_extract(const uint8_t nonce[12]) {
    return ((uint64_t)nonce[6] << 40) | ((uint64_t)nonce[7] << 32) | ((uint64_t)nonce[8] << 24) |
           ((uint64_t)nonce[9] << 16) | ((uint64_t)nonce[10] << 8) | (uint64_t)nonce[11];
}
