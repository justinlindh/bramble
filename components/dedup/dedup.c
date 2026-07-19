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
        /* Evict oldest entry */
        int oldest = 0;
        for (int i = 1; i < buf->count; i++) {
            if (buf->entries[i].timestamp_ms < buf->entries[oldest].timestamp_ms) {
                oldest = i;
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
