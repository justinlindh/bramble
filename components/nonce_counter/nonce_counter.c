#include "include/nonce_counter.h"

static uint32_t s_src;
static uint16_t s_salt;
static uint64_t s_counter;   /* next to issue */
static uint64_t s_ceiling;   /* persisted high water; counters below this are reserved */
static nonce_store_read_fn s_rd;
static nonce_store_write_fn s_wr;
static void* s_ctx;

void nonce_counter_init(uint32_t src_addr, uint16_t boot_salt, nonce_store_read_fn rd,
                        nonce_store_write_fn wr, void* ctx) {
    s_src = src_addr; s_salt = boot_salt; s_rd = rd; s_wr = wr; s_ctx = ctx;
    uint64_t ceiling = 0;
    if (s_rd) s_rd(&ceiling, s_ctx);
    s_counter = ceiling;                 /* resume above anything ever used */
    s_ceiling = ceiling + NONCE_RESERVE;
    if (s_wr) s_wr(s_ceiling, s_ctx);    /* reserve-ahead */
}

int nonce_counter_next(uint8_t nonce_out[12]) {
    uint64_t c = s_counter++;
    if (s_counter >= s_ceiling) {        /* flush-before-cross */
        s_ceiling += NONCE_RESERVE;
        if (s_wr) s_wr(s_ceiling, s_ctx);
    }
    nonce_out[0] = (uint8_t)(s_src >> 24); nonce_out[1] = (uint8_t)(s_src >> 16);
    nonce_out[2] = (uint8_t)(s_src >> 8);  nonce_out[3] = (uint8_t)(s_src);
    nonce_out[4] = (uint8_t)(s_salt >> 8); nonce_out[5] = (uint8_t)(s_salt);
    nonce_out[6]  = (uint8_t)(c >> 40); nonce_out[7]  = (uint8_t)(c >> 32);
    nonce_out[8]  = (uint8_t)(c >> 24); nonce_out[9]  = (uint8_t)(c >> 16);
    nonce_out[10] = (uint8_t)(c >> 8);  nonce_out[11] = (uint8_t)(c);
    return 0;
}

uint64_t nonce_counter_extract(const uint8_t nonce[12]) {
    return ((uint64_t)nonce[6] << 40) | ((uint64_t)nonce[7] << 32) | ((uint64_t)nonce[8] << 24) |
           ((uint64_t)nonce[9] << 16) | ((uint64_t)nonce[10] << 8) | (uint64_t)nonce[11];
}
