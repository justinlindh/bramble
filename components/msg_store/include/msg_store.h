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
    /* Parked by the user: delivery is deferred until the peer rejoins the
     * mesh. Deliberately an added VALUE and not a new field: the persistence
     * backend validates record_size == sizeof(stored_msg_t) and rejects the
     * file on mismatch, so widening the row would discard every device's
     * message history on upgrade. Persistence of the parked state therefore
     * costs nothing: msg_store already writes status changes back to flash. */
    MSG_STATUS_QUEUED = 4,
} msg_status_t;

typedef struct {
    uint32_t peer_addr; /* Remote address (sender or recipient) */
    uint32_t uid;       /* Stable local id for this row, unique within the ring and never 0.
                           A user-submitted message carries its own through the whole send
                           pipeline (route queue, session queue, transmit) so every stage
                           UPDATES this row instead of adding a new one; a row stored
                           without one is given a uid when it is stored, which is what
                           identifies its persisted record. Never leaves the node; not a
                           wire field. */
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
 *
 * Parked is sticky: a row currently MSG_STATUS_QUEUED refuses a transition to
 * MSG_STATUS_FAILED and stays QUEUED instead. A parked message leaves the
 * parked state only by being sent (QUEUED -> SENT or -> DELIVERED, both still
 * apply normally) or by the user cancelling it via msg_store_unpark(), never
 * by a send attempt failing. Without this, every failure path in the send
 * pipeline (payload too large, handshake cap, session queue TTL, ...) already
 * calls this function with MSG_STATUS_FAILED on the same uid it parked,
 * which would silently un-park the row on its very first failed retry and it
 * would never flush again. Every other transition, including SENT ->
 * MSG_STATUS_FAILED for a message that actually reached the air and then
 * exhausted its ACK retries, is unaffected.
 */
bool msg_store_update_by_uid(uint32_t uid, uint32_t packet_id, msg_status_t status);

/**
 * Un-park a message: sets a MSG_STATUS_QUEUED row to MSG_STATUS_FAILED
 * unconditionally, bypassing the sticky rule in msg_store_update_by_uid().
 * This is the only door out of QUEUED that a failed send cannot walk through
 * by accident; it exists so the user's explicit Cancel can still work.
 * Returns false, and changes nothing, if the row is not currently QUEUED or
 * uid is unknown (including uid 0).
 */
bool msg_store_unpark(uint32_t uid);

/**
 * Collect the uids of messages parked for a peer, oldest first.
 *
 * Selects outgoing DMs (channel_index < 0) to peer_addr whose status is
 * MSG_STATUS_QUEUED. Returns the number of uids written, never more than
 * max_out. Returns 0 for a NULL out_uids or a non-positive max_out.
 *
 * The selection rule lives here, not in the mesh task, so it is host-testable
 * and so the flush path stays a loop over uids.
 */
int msg_store_parked_uids_for_peer(uint32_t peer_addr, uint32_t* out_uids, int max_out);

/**
 * Copy the row carrying this uid. Returns false for uid 0 or an unknown uid.
 * The flush path needs a row's text and recipient from its uid; without this
 * every caller would open-code a ring walk, and the ring is circular, so an
 * open-coded walk is a bug waiting to happen.
 */
bool msg_store_get_copy_by_uid(uint32_t uid, stored_msg_t* out);

/**
 * Read just the peer address of the row carrying this uid. Returns false for
 * uid 0, a NULL out, or an unknown uid.
 *
 * A stored_msg_t is over 700 bytes, so a caller that wants nothing but the
 * recipient should not be putting one on a task stack to get it (the flush
 * path keeps its copy static for the same reason). Parking a message runs on
 * whichever task drove the UI or the RPC, and those stacks are not sized for
 * a message-sized frame.
 */
bool msg_store_peer_for_uid(uint32_t uid, uint32_t* out_peer_addr);

/**
 * Rotate to the next peer that has parked messages: the lowest peer address
 * above after_peer_addr, or the lowest of all of them if nothing sorts above
 * it. Returns false only when nothing at all is parked.
 *
 * Passing back the previous answer walks every distinct parked peer in turn
 * and then wraps, which is what lets a caller give each one a turn from a
 * single uint32 of state. Ascending address order is arbitrary but stable,
 * and stable is the property that matters: it cannot starve a peer or visit
 * one twice per lap. Selects the same rows msg_store_parked_uids_for_peer
 * does (outgoing, channel-less, MSG_STATUS_QUEUED).
 */
bool msg_store_next_parked_peer(uint32_t after_peer_addr, uint32_t* out_peer_addr);

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
 * Count of stored outgoing DMs whose delivery receipt has arrived
 * (MSG_DIR_OUTGOING with MSG_STATUS_DELIVERED). Reads the ring, so unlike
 * msg_store_total_incoming() it saturates with it; callers that poll it as a
 * "has any DM confirmed delivery yet" edge trigger (the emulator's scripted
 * sender in emulator/node/emu_autosend.c) are unaffected by that.
 */
uint32_t msg_store_count_outgoing_delivered(void);

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
 * Initialize message store with SPIFFS persistence.
 * If persistence is enabled and messages are found, loads recent messages into RAM.
 * Call this instead of msg_store_init() to enable persistence.
 */
void msg_store_init_with_persistence(void);

#ifdef __cplusplus
}
#endif

#endif /* MSG_STORE_H */
