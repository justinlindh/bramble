#include "dedup.h"
#include <string.h>

void dedup_init(dedup_buffer_t* buf) { memset(buf, 0, sizeof(*buf)); }

bool dedup_check_and_add(dedup_buffer_t* buf, uint32_t packet_id, uint32_t now_ms) {
    /* Purge expired entries first */
    dedup_purge(buf, now_ms);

    /* Check if already present */
    for (int i = 0; i < buf->count; i++) {
        if (buf->entries[i].packet_id == packet_id) {
            return true; /* duplicate */
        }
    }

    /* Not found: add it */
    if (buf->count < DEDUP_MAX_ENTRIES) {
        buf->entries[buf->count].packet_id = packet_id;
        buf->entries[buf->count].timestamp_ms = now_ms;
        buf->count++;
    } else {
        /* Evict the entry with the greatest age. Age is now - timestamp in
         * uint32 arithmetic, as in dedup_purge, so it stays correct across
         * the millisecond counter's wrap where raw timestamps do not. */
        int oldest = 0;
        uint32_t oldest_age = now_ms - buf->entries[0].timestamp_ms;
        for (int i = 1; i < buf->count; i++) {
            uint32_t age = now_ms - buf->entries[i].timestamp_ms;
            if (age > oldest_age) {
                oldest = i;
                oldest_age = age;
            }
        }
        buf->entries[oldest].packet_id = packet_id;
        buf->entries[oldest].timestamp_ms = now_ms;
    }

    return false; /* not duplicate */
}

void dedup_purge(dedup_buffer_t* buf, uint32_t now_ms) {
    int dst = 0;
    for (int src = 0; src < buf->count; src++) {
        if ((now_ms - buf->entries[src].timestamp_ms) < DEDUP_EXPIRY_MS) {
            if (dst != src) {
                buf->entries[dst] = buf->entries[src];
            }
            dst++;
        }
    }
    buf->count = dst;
}

int dedup_count(const dedup_buffer_t* buf) { return buf->count; }

bool dedup_contains(const dedup_buffer_t* buf, uint32_t packet_id, uint32_t now_ms) {
    for (int i = 0; i < buf->count; i++) {
        if (buf->entries[i].packet_id == packet_id &&
            (now_ms - buf->entries[i].timestamp_ms) < DEDUP_EXPIRY_MS) {
            return true;
        }
    }
    return false;
}
