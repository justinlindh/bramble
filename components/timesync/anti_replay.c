#include "anti_replay.h"
#include <string.h>
#include <stdlib.h>

void anti_replay_init(anti_replay_cache_t *cache) {
    memset(cache, 0, sizeof(*cache));
    cache->nonce_counter = 0;
}

bool anti_replay_check(anti_replay_cache_t *cache, uint32_t packet_id,
                       uint32_t packet_time_ms, uint32_t now_ms) {
    // Reject if timestamp outside window
    int64_t diff = (int64_t)packet_time_ms - (int64_t)now_ms;
    if (diff < 0) diff = -diff;
    if (diff > ANTI_REPLAY_WINDOW_MS) {
        return false;
    }

    // Reject if packet_id already in cache
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].packet_id == packet_id) {
            return false;
        }
    }

    return true;
}

void anti_replay_add(anti_replay_cache_t *cache, uint32_t packet_id, uint32_t now_ms) {
    if (cache->count >= ANTI_REPLAY_CACHE_SIZE) {
        // Evict oldest entry
        uint32_t oldest_idx = 0;
        uint32_t oldest_ts = cache->entries[0].timestamp;
        for (int i = 1; i < cache->count; i++) {
            if (cache->entries[i].timestamp < oldest_ts) {
                oldest_ts = cache->entries[i].timestamp;
                oldest_idx = i;
            }
        }
        cache->entries[oldest_idx].packet_id = packet_id;
        cache->entries[oldest_idx].timestamp = now_ms;
    } else {
        cache->entries[cache->count].packet_id = packet_id;
        cache->entries[cache->count].timestamp = now_ms;
        cache->count++;
    }
}

void anti_replay_purge(anti_replay_cache_t *cache, uint32_t now_ms) {
    int write = 0;
    for (int read = 0; read < cache->count; read++) {
        uint32_t age = now_ms - cache->entries[read].timestamp;
        if (age <= ANTI_REPLAY_WINDOW_MS) {
            if (write != read) {
                cache->entries[write] = cache->entries[read];
            }
            write++;
        }
    }
    cache->count = write;
}

uint32_t anti_replay_get_nonce_counter(const anti_replay_cache_t *cache) {
    return cache->nonce_counter;
}

void anti_replay_set_nonce_counter(anti_replay_cache_t *cache, uint32_t value) {
    cache->nonce_counter = value;
}

uint32_t anti_replay_next_nonce(anti_replay_cache_t *cache) {
    return cache->nonce_counter++;
}
