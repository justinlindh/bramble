#include "probe_reply.h"

uint32_t probe_reply_slot_ms(uint32_t address) {
    return 300u + ((address % 6u) * 110u);   /* 300..850 */
}

uint32_t probe_reply_initial_delay_ms(uint32_t address, uint32_t jitter_ms) {
    return probe_reply_slot_ms(address) + jitter_ms;
}

uint32_t probe_reply_attempt_due_ms(uint32_t now_ms, uint32_t initial_delay_ms, uint8_t attempt_index) {
    return now_ms + initial_delay_ms + ((uint32_t)attempt_index * PROBE_REPLY_RETRY_SPACING_MS);
}
