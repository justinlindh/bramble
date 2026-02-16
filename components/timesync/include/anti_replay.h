#ifndef BRAMBLE_ANTI_REPLAY_H
#define BRAMBLE_ANTI_REPLAY_H
#include <stdint.h>
#include <stdbool.h>

#define ANTI_REPLAY_WINDOW_MS 30000
#define ANTI_REPLAY_CACHE_SIZE 64

typedef struct {
    uint32_t packet_id;
    uint32_t timestamp;
} replay_entry_t;

typedef struct {
    replay_entry_t entries[ANTI_REPLAY_CACHE_SIZE];
    int count;
} anti_replay_cache_t;

void anti_replay_init(anti_replay_cache_t *cache);
bool anti_replay_check(anti_replay_cache_t *cache, uint32_t packet_id, uint32_t packet_time_ms, uint32_t now_ms);
void anti_replay_add(anti_replay_cache_t *cache, uint32_t packet_id, uint32_t now_ms);
void anti_replay_purge(anti_replay_cache_t *cache, uint32_t now_ms);
#endif
