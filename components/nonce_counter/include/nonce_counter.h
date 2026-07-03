#ifndef BRAMBLE_NONCE_COUNTER_H
#define BRAMBLE_NONCE_COUNTER_H
#include <stdint.h>

#define NONCE_RESERVE 65536u

typedef int (*nonce_store_read_fn)(uint64_t* ceiling_out, void* ctx);
typedef int (*nonce_store_write_fn)(uint64_t ceiling, void* ctx);

/*
 * Fail-closed contract: a counter value is never issued unless the ceiling
 * covering it has been CONFIRMED durable by a successful write callback.
 * If the reserve-ahead write at init fails, or a boundary flush write fails,
 * the subsystem issues nothing until a write succeeds. This is what makes
 * the "persisted ceiling always exceeds every issued counter" invariant hold
 * even when storage writes are unreliable: nothing is ever issued past what
 * is durably persisted, so a crash can never reveal a counter value NVS
 * doesn't already know about.
 *
 * Not thread-safe: callers with multiple concurrent producers must serialize
 * calls to nonce_counter_next externally (e.g. a mutex around the call
 * site), since this component intentionally has no FreeRTOS/OS dependency
 * to stay host-testable.
 */
int nonce_counter_init(uint32_t src_addr, uint16_t boot_salt, nonce_store_read_fn rd,
                       nonce_store_write_fn wr, void* ctx);

/* Returns 0 and fills nonce_out on success. Returns non-zero and leaves all
 * internal state unchanged (issues nothing) if init never durably succeeded,
 * or if this call needed to flush a new ceiling and that write failed. */
int nonce_counter_next(uint8_t nonce_out[12]);
uint64_t nonce_counter_extract(const uint8_t nonce[12]);
#endif
