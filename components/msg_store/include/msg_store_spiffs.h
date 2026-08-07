#ifndef MSG_STORE_SPIFFS_H
#define MSG_STORE_SPIFFS_H

#include "msg_store.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True when two records describe the same stored message.
 *
 * Compares only the fields an update can never change (the delivery status,
 * packet id and relay path are exactly what does change), so it holds across
 * a status update and across a reload. timestamp_s is deliberately excluded:
 * a restored row's timestamp is zeroed in RAM while the file keeps the
 * original, so comparing it would reject every restored message.
 *
 * This is a drift check, not a lookup key: msg_store_spiffs_update() already
 * knows which record it means and uses this to confirm it before writing.
 */
static inline bool msg_store_record_matches(const stored_msg_t* a, const stored_msg_t* b) {
    if (!a || !b)
        return false;
    return a->uid == b->uid && a->peer_addr == b->peer_addr && a->direction == b->direction &&
           a->channel_index == b->channel_index && a->text_len == b->text_len &&
           memcmp(a->text, b->text, a->text_len) == 0;
}

/**
 * Initialize SPIFFS message persistence.
 * Call after SPIFFS mount, before msg_store operations.
 * Returns 0 on success, -1 on error (e.g., SPIFFS not mounted).
 */
int msg_store_spiffs_init(void);

/**
 * Save a message to SPIFFS (append to file).
 * Returns 0 on success, -1 on error.
 */
int msg_store_spiffs_save(const stored_msg_t* msg);

/**
 * Rewrite an already-persisted record in place, so a delivery status that
 * changes after the message was stored survives a reboot.
 *
 * from_end selects the record counting back from the newest (0 = newest),
 * which is exactly how the RAM ring maps onto the file: both hold a suffix of
 * the same append sequence, so the n-th newest row is the n-th newest record.
 *
 * The stored record is read back and checked with msg_store_record_matches()
 * before anything is written. If the mapping has drifted (an append that
 * failed, a keep-count smaller than the RAM ring) the update is dropped
 * instead of stamping a status onto another message. The persisted
 * timestamp_s is preserved, because a restored row carries none.
 *
 * Returns 0 when the record was rewritten, -1 otherwise.
 */
int msg_store_spiffs_update(int from_end, const stored_msg_t* msg);

/**
 * Get total number of persisted messages.
 */
int msg_store_spiffs_get_count(void);

/**
 * Load messages from SPIFFS into provided buffer.
 * Loads the most recent messages up to max_count.
 * Returns number of messages loaded (up to max_count).
 */
int msg_store_spiffs_load_recent(stored_msg_t* msgs, int max_count);

/**
 * Trigger rollover if message count exceeds max_messages.
 * Keeps newest (max_messages * keep_pct / 100) messages.
 * keep_pct should be between 50-90.
 */
void msg_store_spiffs_rollover(int max_messages, int keep_pct);

/**
 * Clear all persisted messages.
 */
void msg_store_spiffs_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MSG_STORE_SPIFFS_H */
