#ifndef MSG_STORE_SPIFFS_H
#define MSG_STORE_SPIFFS_H

#include "msg_store.h"

#ifdef __cplusplus
extern "C" {
#endif

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
