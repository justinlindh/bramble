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

/* The channel_index a DM (or any channel-less message) is stored under. A DM is
 * peer-keyed, not channel-keyed; a negative channel index is the sole marker
 * that tells a DM apart from channel traffic (both share the INCOMING/OUTGOING
 * directions). Storing a real channel index (0 is the unicast/broadcast default)
 * files the DM under that channel and hides it from its own thread: that was
 * bug F1. msg_store_add_dm() / msg_store_add_channel() own this convention so no
 * caller has to remember it. */
#define MSG_STORE_DM_CHANNEL ((int16_t)-1)

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
    uint32_t uid;       /* Stable local id for one user-submitted message (0 = untracked).
                           Survives the whole send pipeline (route queue, session queue,
                           transmit) so every stage UPDATES this row instead of adding
                           a new one. Never leaves the node; not a wire field. */
    msg_direction_t direction;
    msg_status_t status;     /* Delivery status (outgoing only) */
    uint32_t packet_id;      /* Packet ID for ACK correlation */
    uint32_t timestamp_s;    /* Uptime seconds when stored */
    int8_t rssi;             /* RX RSSI (0 for outgoing) */
    int8_t snr;              /* RX SNR (0 for outgoing) */
    int16_t channel_index;   /* MSG_STORE_DM_CHANNEL (<0) = DM/none, >=0 = channel */
    uint8_t route_hop_count; /* 0 = unavailable */
    uint32_t route_hops[MSG_ROUTE_MAX_HOPS]; /* source->...->destination */
    uint16_t text_len;
    char text[MSG_TEXT_MAX];
} stored_msg_t;

/**
 * Channel index to store a received message under.
 *
 * A DM is peer-keyed, not channel-keyed. Channel traffic and DMs share the
 * INCOMING/OUTGOING directions, so channel_index is the only thing that tells
 * them apart, and both readers key on it being negative for a DM:
 * chat_target_matches_message() requires channel_index < 0 for the DM thread,
 * and chat_unread attributes an incoming message to a DM only when it is < 0.
 *
 * A DM arrives on channel_id 0 (the unicast default; only channel_id > 0 is a
 * real channel message), so storing the raw channel_id files every DM under
 * channel 0: invisible in its own thread, and counted against channel 0's
 * unread badge. Received DMs must store MSG_STORE_DM_CHANNEL.
 *
 * Prefer msg_store_add_dm() / msg_store_add_channel(), which pick the right store
 * entry from the message kind; this helper remains for the readers that already
 * classify a received channel_id.
 */
static inline int16_t msg_store_rx_channel_index(int channel_id) {
    return (channel_id > 0) ? (int16_t)channel_id : MSG_STORE_DM_CHANNEL;
}

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

/* Extended API with channel index metadata. Prefer msg_store_add_dm /
 * msg_store_add_channel below, which own the DM/channel convention; this stays
 * as the shared implementation they call. */
void msg_store_add_ex2(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                       int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status,
                       int16_t channel_index);

/* Store a direct message (or any channel-less message): forces channel_index =
 * MSG_STORE_DM_CHANNEL so it can never be misfiled under a channel. THE way to
 * store a DM. A received broadcast is also channel-less (it is filed by
 * direction, not channel index) and uses this too. */
void msg_store_add_dm(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                      int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status);

/* Store a channel message. channel_index is non-negative by type (0 is the
 * broadcast channel, >0 a named channel); a negative index would mean "DM", and
 * the uint8_t parameter makes that unrepresentable at the call site. THE way to
 * store channel/broadcast traffic. */
void msg_store_add_channel(uint32_t peer_addr, msg_direction_t dir, const char* text,
                           size_t text_len, int8_t rssi, int8_t snr, uint32_t packet_id,
                           msg_status_t status, uint8_t channel_index);

/* Convenience wrapper (no ACK tracking) */
void msg_store_add(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                   int8_t rssi, int8_t snr);

/**
 * Allocate a stable uid for one user-submitted message. Monotonic, never 0.
 * Reset only by msg_store_init().
 */
uint32_t msg_store_next_uid(void);

/**
 * Store a DM carrying a stable uid (uid 0 behaves exactly like msg_store_add_dm).
 * The send pipeline calls this ONCE per user message, at whichever stage first
 * sees it, and every later stage reconciles that row with
 * msg_store_update_by_uid() instead of adding another.
 */
void msg_store_add_dm_uid(uint32_t peer_addr, msg_direction_t dir, const char* text,
                          size_t text_len, int8_t rssi, int8_t snr, uint32_t packet_id,
                          msg_status_t status, uint32_t uid);

/**
 * Update the row carrying this uid: sets status, and sets packet_id too when a
 * nonzero packet_id is passed (0 = leave the stored packet_id alone). This is
 * how a later pipeline stage (session flush, transmit) stamps the real wire
 * packet_id onto the row an earlier stage already created, so ACK correlation
 * (which is still by packet_id) lands on that one row.
 * Returns true if a row with this uid was found. uid 0 never matches.
 */
bool msg_store_update_by_uid(uint32_t uid, uint32_t packet_id, msg_status_t status);

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
