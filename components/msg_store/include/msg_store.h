#ifndef MSG_STORE_H
#define MSG_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#ifdef CONFIG_BRAMBLE_MSG_STORE_CAP
#define MSG_STORE_MAX CONFIG_BRAMBLE_MSG_STORE_CAP
#else
#define MSG_STORE_MAX 20
#endif
#define MSG_TEXT_MAX 640
#define MSG_ROUTE_MAX_HOPS 10

typedef enum {
    MSG_DIR_INCOMING = 0,
    MSG_DIR_OUTGOING = 1,
    MSG_DIR_BROADCAST_IN = 2,
    MSG_DIR_BROADCAST_OUT = 3,
} msg_direction_t;

typedef enum {
    MSG_STATUS_NONE = 0,      /* No delivery tracking (incoming/broadcast) */
    MSG_STATUS_SENT = 1,      /* Transmitted over radio */
    MSG_STATUS_DELIVERED = 2, /* ACK received from recipient */
    MSG_STATUS_FAILED = 3,    /* Max retries exhausted */
} msg_status_t;

typedef struct {
    uint32_t peer_addr; /* Remote address (sender or recipient) */
    msg_direction_t direction;
    msg_status_t status;                     /* Delivery status (outgoing only) */
    uint32_t packet_id;                      /* Packet ID for ACK correlation */
    uint32_t timestamp_s;                    /* Uptime seconds when stored */
    int8_t rssi;                             /* RX RSSI (0 for outgoing) */
    int8_t snr;                              /* RX SNR (0 for outgoing) */
    int16_t channel_index;                   /* -1 = none/broadcast, >=0 = channel */
    uint8_t route_hop_count;                 /* 0 = unavailable */
    uint32_t route_hops[MSG_ROUTE_MAX_HOPS]; /* source->...->destination */
    uint16_t text_len;
    char text[MSG_TEXT_MAX];
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
void msg_store_add_ex(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                      int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status);

/* Extended API with channel index metadata */
void msg_store_add_ex2(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                       int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status,
                       int16_t channel_index);

/* Convenience wrapper (no ACK tracking) */
void msg_store_add(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                   int8_t rssi, int8_t snr);

/**
 * Update delivery status for a message by packet_id.
 * Returns true if found and updated.
 */
bool msg_store_update_status(uint32_t packet_id, msg_status_t status);

/**
 * Update status and optional relay path (source->...->destination) by packet_id.
 * Pass route_hops=NULL or route_hop_count=0 to only update status.
 */
bool msg_store_update_status_with_route(uint32_t packet_id, msg_status_t status,
                                        uint8_t route_hop_count, const uint32_t* route_hops);

/**
 * Get total number of stored messages.
 */
int msg_store_count(void);

/**
 * Monotonic count of INCOMING messages ever stored (MSG_DIR_INCOMING or
 * MSG_DIR_BROADCAST_IN). Unlike msg_store_count(), this never saturates at
 * the ring capacity, so callers can detect arrivals by delta. Reset only by
 * msg_store_init().
 */
uint32_t msg_store_total_incoming(void);

/**
 * Get message at index (0 = oldest).  Returns NULL if out of range.
 * Returned pointer is valid until next msg_store_add.
 *
 * NOTE: not concurrency-safe. The returned pointer aliases the live ring, so
 * a concurrent writer (mesh task) can overwrite the slot while the caller
 * reads it. Prefer msg_store_get_copy() from the UI task.
 */
const stored_msg_t* msg_store_get(int index);

/**
 * Copy the message at index (0 = oldest) into caller-owned storage under the
 * store lock. Returns true on success, false if out of range or unallocated.
 * The copy is a stable snapshot: safe to read across LVGL calls even while the
 * mesh task keeps writing the ring. This is the concurrency-safe reader.
 */
bool msg_store_get_copy(int index, stored_msg_t* out);

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
