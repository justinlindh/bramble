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
    uint32_t nonce_counter;
} anti_replay_cache_t;

void anti_replay_init(anti_replay_cache_t* cache);
bool anti_replay_check(anti_replay_cache_t* cache, uint32_t packet_id, uint32_t packet_time_ms,
                       uint32_t now_ms);
void anti_replay_add(anti_replay_cache_t* cache, uint32_t packet_id, uint32_t now_ms);
void anti_replay_purge(anti_replay_cache_t* cache, uint32_t now_ms);

/* Nonce counter for reboot-safe anti-replay.
   Call get before shutdown to persist to NVS.
   Call set on boot with persisted value + safety margin. */
uint32_t anti_replay_get_nonce_counter(const anti_replay_cache_t* cache);
void anti_replay_set_nonce_counter(anti_replay_cache_t* cache, uint32_t value);
uint32_t anti_replay_next_nonce(anti_replay_cache_t* cache);
#endif
