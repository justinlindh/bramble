#include "probe_reply.h"
#include <string.h>

uint32_t probe_reply_slot_ms(uint32_t address) {
    return 300u + ((address % 6u) * 110u); /* 300..850 */
}

uint32_t probe_reply_initial_delay_ms(uint32_t address, uint32_t jitter_ms) {
    return probe_reply_slot_ms(address) + jitter_ms;
}

uint32_t probe_reply_attempt_due_ms(uint32_t now_ms, uint32_t initial_delay_ms,
                                    uint8_t attempt_index) {
    return now_ms + initial_delay_ms + ((uint32_t)attempt_index * PROBE_REPLY_RETRY_SPACING_MS);
}

int probe_reply_queue_insert(pending_probe_reply_t* q, int cap, const uint8_t* buf,
                             uint8_t wire_len, uint8_t attempts_total, uint32_t first_due_ms) {
    int slot = -1;
    for (int i = 0; i < cap; i++) {
        if (!q[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    pending_probe_reply_t* item = &q[slot];
    if (wire_len > sizeof(item->buf))
        return -1;
    memset(item, 0, sizeof(*item));
    item->used = true;
    memcpy(item->buf, buf, wire_len);
    item->wire_len = wire_len;
    item->attempts_total = attempts_total;
    item->attempts_sent = 0;
    item->due_at_ms = first_due_ms;
    return slot;
}

bool probe_reply_queue_earliest_due(const pending_probe_reply_t* q, int cap,
                                    uint32_t* earliest_due_ms) {
    bool have = false;
    uint32_t earliest = 0;
    for (int i = 0; i < cap; i++) {
        if (!q[i].used)
            continue;
        if (!have || q[i].due_at_ms < earliest) {
            earliest = q[i].due_at_ms;
            have = true;
        }
    }
    if (have && earliest_due_ms)
        *earliest_due_ms = earliest;
    return have;
}

int probe_reply_queue_find_due(const pending_probe_reply_t* q, int cap, uint32_t now_ms) {
    for (int i = 0; i < cap; i++) {
        if (q[i].used && q[i].due_at_ms <= now_ms)
            return i;
    }
    return -1;
}

void probe_reply_queue_apply_result(pending_probe_reply_t* item, probe_reply_tx_result_t result,
                                    uint32_t now_ms) {
    if (result == PROBE_REPLY_TX_DENIED) {
        memset(item, 0, sizeof(*item)); /* deny-stop: abandon the whole reply */
        return;
    }
    item->attempts_sent++;
    if (item->attempts_sent >= item->attempts_total) {
        memset(item, 0, sizeof(*item));
        return;
    }
    item->due_at_ms = now_ms + PROBE_REPLY_RETRY_SPACING_MS;
}
