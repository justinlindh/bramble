#ifndef BRAMBLE_NONCE_COUNTER_H
#define BRAMBLE_NONCE_COUNTER_H
#include <stdint.h>

#define NONCE_RESERVE 65536u

typedef int (*nonce_store_read_fn)(uint64_t* ceiling_out, void* ctx);
typedef int (*nonce_store_write_fn)(uint64_t ceiling, void* ctx);

void nonce_counter_init(uint32_t src_addr, uint16_t boot_salt, nonce_store_read_fn rd,
                        nonce_store_write_fn wr, void* ctx);
int nonce_counter_next(uint8_t nonce_out[12]);
uint64_t nonce_counter_extract(const uint8_t nonce[12]);
#endif
