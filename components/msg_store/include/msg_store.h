#ifndef MSG_STORE_H
#define MSG_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSG_STORE_MAX       20
#define MSG_TEXT_MAX        200

typedef enum {
    MSG_DIR_INCOMING = 0,
    MSG_DIR_OUTGOING = 1,
    MSG_DIR_BROADCAST_IN = 2,
    MSG_DIR_BROADCAST_OUT = 3,
} msg_direction_t;

typedef enum {
    MSG_STATUS_NONE = 0,       /* No delivery tracking (incoming/broadcast) */
    MSG_STATUS_SENT = 1,       /* Transmitted over radio */
    MSG_STATUS_DELIVERED = 2,  /* ACK received from recipient */
    MSG_STATUS_FAILED = 3,     /* Max retries exhausted */
} msg_status_t;

typedef struct {
    uint32_t        peer_addr;      /* Remote address (sender or recipient) */
    msg_direction_t direction;
    msg_status_t    status;         /* Delivery status (outgoing only) */
    uint32_t        packet_id;      /* Packet ID for ACK correlation */
    uint32_t        timestamp_s;    /* Uptime seconds when stored */
    int8_t          rssi;           /* RX RSSI (0 for outgoing) */
    int8_t          snr;            /* RX SNR (0 for outgoing) */
    int16_t         channel_index;  /* -1 = none/broadcast, >=0 = channel */
    uint16_t        text_len;
    char            text[MSG_TEXT_MAX];
} stored_msg_t;

/**
 * Initialize the message store.  Call once at startup.
 */
void msg_store_init(void);

/**
 * Add a message to the store.  If full, the oldest message is evicted.
 * text is copied internally (truncated to MSG_TEXT_MAX-1).
 * packet_id is used for ACK correlation (0 = no tracking).
 */
void msg_store_add_ex(uint32_t peer_addr, msg_direction_t dir,
                      const char *text, size_t text_len,
                      int8_t rssi, int8_t snr,
                      uint32_t packet_id, msg_status_t status);

/* Extended API with channel index metadata */
void msg_store_add_ex2(uint32_t peer_addr, msg_direction_t dir,
                       const char *text, size_t text_len,
                       int8_t rssi, int8_t snr,
                       uint32_t packet_id, msg_status_t status,
                       int16_t channel_index);

/* Convenience wrapper (no ACK tracking) */
void msg_store_add(uint32_t peer_addr, msg_direction_t dir,
                   const char *text, size_t text_len,
                   int8_t rssi, int8_t snr);

/**
 * Update delivery status for a message by packet_id.
 * Returns true if found and updated.
 */
bool msg_store_update_status(uint32_t packet_id, msg_status_t status);

/**
 * Get total number of stored messages.
 */
int msg_store_count(void);

/**
 * Get message at index (0 = oldest).  Returns NULL if out of range.
 * Returned pointer is valid until next msg_store_add.
 */
const stored_msg_t *msg_store_get(int index);

/**
 * Clear all stored messages.
 */
void msg_store_clear(void);

/**
 * Initialize message store with SPIFFS persistence.
 * If persistence is enabled and messages are found, loads recent messages into RAM.
 * Call this instead of msg_store_init() to enable persistence.
 */
void msg_store_init_with_persistence(void);

#ifdef __cplusplus
}
#endif

#endif /* MSG_STORE_H */
