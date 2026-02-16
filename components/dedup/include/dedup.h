#ifndef BRAMBLE_DEDUP_H
#define BRAMBLE_DEDUP_H
#include <stdint.h>
#include <stdbool.h>
#define DEDUP_MAX_ENTRIES 256
#define DEDUP_EXPIRY_MS   60000
typedef struct { uint32_t packet_id; uint32_t timestamp_ms; } dedup_entry_t;
typedef struct { dedup_entry_t entries[DEDUP_MAX_ENTRIES]; int count; } dedup_buffer_t;
void dedup_init(dedup_buffer_t *buf);
bool dedup_check_and_add(dedup_buffer_t *buf, uint32_t packet_id, uint32_t now_ms);
void dedup_purge(dedup_buffer_t *buf, uint32_t now_ms);
int dedup_count(const dedup_buffer_t *buf);
#endif
