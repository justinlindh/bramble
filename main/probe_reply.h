#ifndef PROBE_REPLY_H
#define PROBE_REPLY_H

#include <stdint.h>

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

#endif /* PROBE_REPLY_H */
