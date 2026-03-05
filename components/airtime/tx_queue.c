#include "tx_queue.h"
#include <string.h>

void tx_queue_init(tx_queue_t* q) { memset(q, 0, sizeof(*q)); }

int tx_queue_enqueue(tx_queue_t* q, const uint8_t* data, uint16_t len, uint8_t priority,
                     uint32_t now_ms) {
    if (len > 256)
        return -1;

    if (q->count >= TX_QUEUE_SIZE) {
        // Find lowest priority, oldest entry to drop
        int victim = -1;
        for (int i = 0; i < q->count; i++) {
            if (!q->entries[i].active)
                continue;
            if (victim == -1) {
                victim = i;
                continue;
            }
            if (q->entries[i].priority < q->entries[victim].priority ||
                (q->entries[i].priority == q->entries[victim].priority &&
                 q->entries[i].enqueue_time < q->entries[victim].enqueue_time)) {
                victim = i;
            }
        }
        if (victim == -1 || q->entries[victim].priority > priority)
            return -1;
        // Remove victim and compact
        for (int i = victim; i < q->count - 1; i++) {
            q->entries[i] = q->entries[i + 1];
        }
        q->count--;
    }

    tx_entry_t* e = &q->entries[q->count];
    memcpy(e->data, data, len);
    e->len = len;
    e->priority = priority;
    e->enqueue_time = now_ms;
    e->active = true;
    q->count++;
    return 0;
}

bool tx_queue_dequeue(tx_queue_t* q, uint8_t* data, uint16_t* len) {
    if (q->count == 0)
        return false;

    // Find highest priority (oldest if tie)
    int best = -1;
    for (int i = 0; i < q->count; i++) {
        if (!q->entries[i].active)
            continue;
        if (best == -1) {
            best = i;
            continue;
        }
        if (q->entries[i].priority > q->entries[best].priority ||
            (q->entries[i].priority == q->entries[best].priority &&
             q->entries[i].enqueue_time < q->entries[best].enqueue_time)) {
            best = i;
        }
    }
    if (best == -1)
        return false;

    memcpy(data, q->entries[best].data, q->entries[best].len);
    *len = q->entries[best].len;

    // Compact
    for (int i = best; i < q->count - 1; i++) {
        q->entries[i] = q->entries[i + 1];
    }
    q->count--;
    return true;
}

int tx_queue_count(const tx_queue_t* q) { return q->count; }

bool tx_queue_is_full(const tx_queue_t* q) { return q->count >= TX_QUEUE_SIZE; }
