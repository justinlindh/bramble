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

/* How old a message may get before the node stops trying to deliver it, in
 * seconds of uptime.
 *
 * Measured from when the message was STORED, not from when it was parked:
 * timestamp_s is stamped once by msg_store_add_full and nothing restamps it.
 * So this bounds the whole life of a message that will not go through, which
 * is the interval that matters for the peer's thread and for airtime, and a
 * row parked late gets whatever is left of the window rather than a fresh one.
 * msg_store_park_window_open exists so a row with nothing left cannot be
 * parked at all, instead of being parked and cancelled minutes later.
 *
 * A parked row is retried once per PARKED_RETRY_COOLDOWN_MS (main/parked_retry.h,
 * 300s) for as long as it stays parked, and every retry that reaches the peer
 * renders there: each carries a fresh packet_id, and the receiver's duplicate
 * suppression is packet_id-keyed over a 60s window, so consecutive retries
 * never look like the same frame. On an asymmetric link, where this node's
 * transmit arrives but the peer's ACK does not, that is one visible copy per
 * cooldown with nothing ending it. The bound is therefore worth roughly
 * TTL / cooldown copies in the worst case, and at two hours that is 24.
 *
 * Two hours is the trade, not a round number. The feature exists for a peer who
 * is out of range for a while, so a short TTL guts it: at one hour the ceiling
 * only halves, to 12, while the window stops covering an ordinary walk out of
 * range and back. Going the other way, anything that covers "overnight" implies
 * 96 copies at eight hours and 288 at a day, and a thread with 288 copies of
 * one message in it is a worse outcome than the message failing. 24 is a number
 * a user can read as a broken link rather than as the app destroying the
 * conversation, and expiry is not a loss: the row becomes visibly FAILED, which
 * is still retryable and still re-parkable by hand.
 *
 * The ring's own eviction does not substitute for this. It bounds only a BUSY
 * node, arrives at a load-dependent time nobody can predict, and its outcome is
 * strictly worse: the row leaves history entirely rather than reporting that it
 * was not delivered. On a quiet node, which is the normal case for a small
 * mesh, eviction may never happen at all.
 *
 * UPTIME, NOT WALL CLOCK. timestamp_s comes from the boot-relative uptime
 * clock, and msg_store_load_from_flash deliberately zeroes restored timestamps
 * because a previous boot's uptime is meaningless in this one. So a reboot
 * restarts a parked row's TTL from zero. That is accepted: the point is to
 * bound one continuous stretch of retrying, and a node that reboots has already
 * stopped retrying for as long as it was down. */
#define MSG_STORE_PARK_TTL_S 7200u

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
 * Parked is sticky: MSG_STATUS_DELIVERED is the ONLY status a row currently
 * MSG_STATUS_QUEUED will accept. Anything else, including MSG_STATUS_SENT, is
 * refused and the row stays QUEUED. A parked message therefore leaves the
 * parked state exactly three ways: it is genuinely delivered, the user cancels
 * it via msg_store_unpark(), or it ages out of the park window and
 * msg_store_expire_parked() gives up on it. The last two write the row's status
 * directly rather than coming through here, which is what keeps this rule from
 * blocking the two deliberate exits.
 *
 * SENT is refused rather than allowed, which is the part that is easy to get
 * wrong. Marking a parked row SENT looks like honest progress, and it is fatal:
 * an attempt that reaches the air but is never acknowledged ends with the ACK
 * retry tick reporting MSG_STATUS_FAILED against that packet_id, and a row
 * sitting at SENT is no longer protected by this rule, so it goes FAILED. Since
 * mesh_park_message is the only producer of QUEUED anywhere in the tree,
 * nothing would ever re-park it and the message is stranded after exactly ONE
 * attempt, under a promise to the user that it will keep trying. An
 * unacknowledged attempt is the normal outcome on a marginal link, which is the
 * very situation parking exists for.
 *
 * Staying QUEUED across the transmit costs nothing, because the packet_id this
 * function stamps is written before the status is considered: ACK correlation
 * still lands on the row, so a real delivery still resolves it. What it means
 * for the caller is that a parked row's status does not report attempt
 * progress, only the outcome, and the badge keeps telling the user the truth,
 * which is that the message is still waiting.
 *
 * Without the rule, every failure path in the send pipeline (payload too large,
 * handshake cap, session queue TTL, ACK exhaustion) calls this function with
 * MSG_STATUS_FAILED on the same uid it parked. Transitions between non-QUEUED
 * statuses, including SENT -> FAILED for an ordinary unparked message, are
 * unaffected.
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
 * MSG_STATUS_QUEUED and whose age against now_s is under MSG_STORE_PARK_TTL_S.
 * Returns the number of uids written, never more than max_out. Returns 0 for a
 * NULL out_uids or a non-positive max_out.
 *
 * now_s is the caller's uptime in seconds, the same clock timestamp_s is
 * stamped from. Passed in rather than read here so the rule is testable on a
 * host, where that clock does not run.
 *
 * A row past the TTL is skipped but NOT changed here: this is a pure read, and
 * a selector that quietly rewrote rows would be the "mutating query" hazard
 * this feature has already been bitten by. msg_store_expire_parked is what
 * makes the row visibly failed. Skipping here as well as there means the
 * beacon path cannot re-send an expired row in the window before the next
 * expiry pass runs.
 *
 * The selection rule lives here, not in the mesh task, so it is host-testable
 * and so the flush path stays a loop over uids.
 */
int msg_store_parked_uids_for_peer(uint32_t peer_addr, uint32_t* out_uids, int max_out,
                                   uint32_t now_s);

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
 * does (outgoing, channel-less, MSG_STATUS_QUEUED, inside the park TTL), so a
 * peer whose only parked rows have expired does not consume a turn.
 */
bool msg_store_next_parked_peer(uint32_t after_peer_addr, uint32_t* out_peer_addr, uint32_t now_s);

/**
 * Give up on every parked row older than MSG_STORE_PARK_TTL_S: move it to
 * MSG_STATUS_FAILED and persist that. Returns how many rows it expired.
 *
 * Separate from the selectors on purpose. They must not mutate, and this must,
 * because an expired row has to become VISIBLY failed rather than silently
 * stop being retried: the user was told the message would be sent when the peer
 * came back, so when the node stops trying, the thread has to stop saying that.
 * A failed row is the right end state, since it is still retryable and still
 * re-parkable by hand.
 *
 * Writes the row's status directly, as msg_store_unpark does, so it is not
 * blocked by the sticky
 * rule that (correctly) refuses QUEUED -> FAILED from a send path.
 *
 * Call it on a periodic tick, not per packet: it walks the ring.
 */
int msg_store_expire_parked(uint32_t now_s);

/**
 * Could parking the row carrying this uid still achieve anything: is it inside
 * the park window measured from when the message was stored? False for uid 0
 * and for an unknown uid.
 *
 * Ask BEFORE parking. The window runs from the message's own age, not from the
 * moment it is parked, so a message that failed and then sat unattended for
 * longer than MSG_STORE_PARK_TTL_S is already past it. Parking such a row would
 * promise a retry that the next expiry pass cancels within one sweep interval,
 * having never selected it once: the exact "we said we would send it" failure
 * this whole feature exists to remove, arriving through a new door. Refusing up
 * front lets the caller say something true instead.
 *
 * This is about age alone. Whether the row is a failed outgoing DM at all is
 * the UI's question (chat_message_is_parkable).
 */
bool msg_store_park_window_open(uint32_t uid, uint32_t now_s);

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
