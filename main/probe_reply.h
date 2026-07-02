#ifndef PROBE_REPLY_H
#define PROBE_REPLY_H

#include <stdint.h>
#include <stdbool.h>

#define PROBE_REPLY_ATTEMPTS 3
#define PROBE_REPLY_RETRY_SPACING_MS 140

/* Deterministic per-node slot delay for probe replies: 300 + (address%6)*110,
 * i.e. 300..850ms. Mirrors the pre-fix mesh_task.c slot math exactly. */
uint32_t probe_reply_slot_ms(uint32_t address);

/* Full initial delay for the first reply attempt: slot + caller jitter (0..119). */
uint32_t probe_reply_initial_delay_ms(uint32_t address, uint32_t jitter_ms);

/* Absolute due time (ms) for a 0-based attempt index: attempt 0 fires at
 * now + initial_delay; each subsequent attempt is PROBE_REPLY_RETRY_SPACING_MS later. */
uint32_t probe_reply_attempt_due_ms(uint32_t now_ms, uint32_t initial_delay_ms, uint8_t attempt_index);

#define PROBE_REPLY_QUEUE_CAPACITY 4

typedef struct {
    bool     used;
    uint8_t  buf[20];
    uint8_t  wire_len;
    uint8_t  attempts_total;
    uint8_t  attempts_sent;
    uint32_t due_at_ms;
} pending_probe_reply_t;

typedef enum {
    PROBE_REPLY_TX_DENIED = 0,   /* budget denied: abandon the whole reply */
    PROBE_REPLY_TX_SENT   = 1,   /* sent OK: count it, then retry or complete */
} probe_reply_tx_result_t;

/* Insert one reply into the first free slot. Returns slot index, or -1 if
 * the queue is full (overflow reply dropped). */
int probe_reply_queue_insert(pending_probe_reply_t *q, int cap,
                             const uint8_t *buf, uint8_t wire_len,
                             uint8_t attempts_total, uint32_t first_due_ms);

/* Writes the smallest due_at_ms among used slots to *earliest_due_ms.
 * Returns true if any slot is pending, false if the queue is empty. */
bool probe_reply_queue_earliest_due(const pending_probe_reply_t *q, int cap,
                                    uint32_t *earliest_due_ms);

/* Returns the first used slot whose due_at_ms <= now_ms, or -1 if none due. */
int probe_reply_queue_find_due(const pending_probe_reply_t *q, int cap, uint32_t now_ms);

/* Apply a TX result to a slot. DENIED frees the slot (deny-stop: the whole
 * reply is abandoned on budget denial). SENT increments attempts_sent; if it
 * reaches attempts_total the slot is freed, else due_at_ms is reset to
 * now_ms + PROBE_REPLY_RETRY_SPACING_MS. */
void probe_reply_queue_apply_result(pending_probe_reply_t *item,
                                    probe_reply_tx_result_t result, uint32_t now_ms);

#endif /* PROBE_REPLY_H */
