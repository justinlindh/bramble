#ifndef BRAMBLE_LOCATION_H
#define BRAMBLE_LOCATION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Privacy tiers */
#define LOCATION_TIER_FULL 0     /* lat/lon/alt/speed/heading */
#define LOCATION_TIER_COARSE 1   /* ~1km grid square */
#define LOCATION_TIER_PRESENCE 2 /* online/offline only */

/* Packet sizes */
#define LOCATION_FULL_SIZE 17    /* lat(4)+lon(4)+alt(2)+acc(1)+spd(1)+hdg(1)+ts(4) */
#define LOCATION_COARSE_SIZE 5   /* grid_lat(2)+grid_lon(2)+ts_low(1) */
#define LOCATION_PRESENCE_SIZE 1 /* status(1) */

/*
 * SEC-C1 (Task 2.1, RFC section 2, M11): the sharing tier moves into the
 * encrypted plaintext instead of the cleartext header flags, and every
 * tier is padded to one canonical inner size so an observer cannot infer
 * the tier (and therefore how much a sender trusts the recipient) from
 * ciphertext length. LOCATION_INNER_TIER_OFFSET is the byte offset of the
 * tier prefix within the inner plaintext (channel path: right after
 * channel_msg's own CHANNEL_MSG_OVERHEAD framing; session path: byte 0,
 * dm_session_encrypt has no framing of its own). L_LOC_INNER is that
 * tier(1) prefix plus the position payload padded up to LOCATION_FULL_SIZE
 * (the largest tier), so every tier (PRESENCE/COARSE/FULL) produces the
 * same L_LOC_INNER-byte plaintext regardless of how few real bytes the
 * chosen tier actually needs.
 */
#define LOCATION_INNER_TIER_OFFSET 0
#define L_LOC_INNER (1 + LOCATION_FULL_SIZE) /* 18: tier(1) + padded position payload */

#define LOCATION_MAX_CONTACTS 16
#define LOCATION_DEFAULT_INTERVAL_S 300 /* 5 minutes */
#define LOCATION_MIN_INTERVAL_S 30      /* floor for periodic sharing */
#define LOCATION_CACHE_TTL_MS 3600000   /* 1 hour */

/* Persistent sharing policy */
typedef struct {
    bool enabled;
    uint8_t default_tier; /* LOCATION_TIER_* */
    uint16_t interval_s;
} location_policy_t;

/* Position data */
typedef struct {
    int32_t latitude_e7;  /* degrees * 1e7 */
    int32_t longitude_e7; /* degrees * 1e7 */
    int16_t altitude_m;
    uint8_t accuracy_m; /* 0-255 meters */
    uint8_t speed_kmh;
    uint8_t heading_deg2; /* heading / 2 (0-179 = 0-358) */
    uint32_t timestamp;   /* epoch seconds, truncated */
    bool valid;
} bramble_position_t;

/* Cached position from peer */
typedef struct {
    uint32_t peer_addr;
    bramble_position_t pos;
    uint32_t received_ms;
    bool active;
} location_cache_entry_t;

/* Location manager state */
typedef struct {
    bramble_position_t my_position;
    location_cache_entry_t cache[LOCATION_MAX_CONTACTS];
    int cache_count;
} location_manager_t;

/* Location sharing mode for Settings UI */
typedef enum {
    LOC_SHARE_OFF = 0,    /* no location shared */
    LOC_SHARE_COARSE = 1, /* ~1km grid square */
    LOC_SHARE_EXACT = 2,  /* precise GPS */
    LOC_SHARE_COUNT = 3
} loc_share_mode_t;

/* NVS-backed getter/setter: persisted in namespace "bramble", key "loc_share" */
loc_share_mode_t location_share_mode_get(void);
void location_share_mode_set(loc_share_mode_t mode);

/* Init */
void location_init(location_manager_t* mgr);

/* Position update */
void location_set_position(location_manager_t* mgr, const bramble_position_t* pos);

/* Serialization */
int location_serialize_full(const bramble_position_t* pos, uint8_t* buf, size_t buf_len);
int location_deserialize_full(const uint8_t* buf, size_t len, bramble_position_t* pos);
int location_serialize_coarse(const bramble_position_t* pos, uint8_t* buf, size_t buf_len);
int location_deserialize_coarse(const uint8_t* buf, size_t len, bramble_position_t* pos);
int location_serialize_presence(const bramble_position_t* pos, uint8_t* buf, size_t buf_len);
int location_serialize_for_tier(const bramble_position_t* pos, uint8_t tier, uint8_t* buf,
                                size_t buf_len);

/*
 * SEC-C1 RX (Task 2.2): the decrypt-mechanism-agnostic tail, shared by both
 * handle_location's channel path (after channel_msg_decrypt) and its
 * session path (after dm_session_decrypt). Deliberately kept dependency-free
 * (no channel_msg.h / dm_session.h include here): it only takes
 * already-authenticated plaintext bytes, reads the tier from byte
 * LOCATION_INNER_TIER_OFFSET (never the header flags), and parses the
 * position with the TIER-APPROPRIATE deserialize length, ignoring the
 * trailing canonical L_LOC_INNER pad. Returns 0 on success (fills tier_out
 * and pos_out), -1 on any parse failure. The decrypt-specific glue (trial
 * against channels, or a session lookup) lives in mesh_task.c, which
 * already depends on both channel_msg.h and dm_session.h; adding either as
 * a REQUIRES of the location component here rippled into unrelated host
 * test targets that only need location's other, decrypt-independent API.
 */
int location_parse_inner(const uint8_t* plaintext, size_t plaintext_len, uint8_t* tier_out,
                         bramble_position_t* pos_out);

/* Cache */
int location_cache_update(location_manager_t* mgr, uint32_t peer_addr,
                          const bramble_position_t* pos, uint32_t now_ms);
const location_cache_entry_t* location_cache_get(const location_manager_t* mgr, uint32_t peer_addr);
void location_cache_purge(location_manager_t* mgr, uint32_t now_ms);

/*
 * Share targets.
 *
 * A location share always goes to an explicit target. Two kinds exist and
 * they are not interchangeable:
 *
 *   CONTACT: one peer, unicast under that peer's DM session key, so only
 *     that peer can read it. Needs an ACTIVE session, which in turn needs a
 *     route, so it only works once directed traffic has flowed to that peer.
 *
 *   CHANNEL: everyone holding the channel key, broadcast to
 *     0xFFFFFFFF under that channel key. Needs no session, no route and no
 *     prior traffic, which is what makes group location sharing work on a
 *     mesh whose members have only ever broadcast.
 *
 * A channel target's tier is the resolution EVERY member of that channel
 * receives. Per-contact tiers express "how much I trust this one peer" and
 * have no meaning against a channel audience, so a channel target carries
 * exactly one tier and it is applied to the single broadcast frame.
 *
 * Targets are persisted in the location NVS namespace, one key per target:
 * LOCATION_CONTACT_RULE_PREFIX plus the 8 hex digits of the peer address, or
 * LOCATION_CHANNEL_RULE_PREFIX plus the two-digit channel index. The value is
 * the rule string described at location_rule_parse.
 */
#define LOCATION_CONTACT_RULE_PREFIX "lcr_"
#define LOCATION_CHANNEL_RULE_PREFIX "lch_"

#define LOCATION_TARGET_CONTACT 0
#define LOCATION_TARGET_CHANNEL 1

/* Mirrors MAX_CHANNELS from the channel component. Kept as a literal so this
 * header stays dependency-free; main/mesh_location.c static-asserts the two
 * agree. */
#define LOCATION_MAX_CHANNEL_TARGETS 16
#define LOCATION_MAX_TARGETS (LOCATION_MAX_CONTACTS + LOCATION_MAX_CHANNEL_TARGETS)

/* Mirrors BRAMBLE_PUBLIC_CHANNEL_INDEX, kept as a literal for the same reason
 * as LOCATION_MAX_CHANNEL_TARGETS; main/mesh_location.c static-asserts the two
 * agree. */
#define LOCATION_PUBLIC_CHANNEL_INDEX 0

/*
 * Whether a channel index may carry this node's position.
 *
 * The public channel's PSK is documented as known to everyone, not just to
 * channel members, so a target on it is a periodic broadcast of exact
 * coordinates that any receiver in radio range decrypts. It is worse than it
 * first looks: the shared replay window is deliberately skipped for
 * public-channel decrypts (a forgeable src_addr there could otherwise be used
 * to slam a victim's high-water mark into a DoS), so those position frames can
 * also be replayed at will.
 *
 * Position is not a datum to emit under a well-known key, so this is a hard
 * rejection rather than a warning or an off-by-default toggle. Both the RPC
 * setter and the send path ask this, so a rule already in NVS from an older
 * build cannot transmit either.
 */
bool location_channel_target_is_permitted(int channel_index);

/*
 * Handshake pacing for directed targets.
 *
 * A directed share needs an ACTIVE DM session, and a session only exists after
 * a handshake. A contact target on a peer nobody has ever messaged therefore
 * has no session, and dropping the share leaves that target dead forever. The
 * send path asks for a session instead, but a peer that is powered off, out of
 * range or simply not answering must not draw one handshake attempt per target
 * per share round: with LOCATION_MAX_CONTACTS targets that is real airtime and
 * battery on a LoRa mesh.
 *
 * First attempt fires as soon as a target comes due with no session, then the
 * delay doubles from LOCATION_HS_BACKOFF_START_MS to LOCATION_HS_BACKOFF_MAX_MS.
 * The start is deliberately above LOCATION_MIN_INTERVAL_S: the share interval
 * floor is 30s, so a shorter gate would pass on every single tick and gate
 * nothing. Clearing a peer on session establishment returns it to the fast
 * first attempt, so a peer that drops out and returns recovers quickly.
 *
 * Pure state, no radio and no locks, so the pacing is host-testable on its own.
 */
#define LOCATION_HS_BACKOFF_START_MS 60000U
#define LOCATION_HS_BACKOFF_MAX_MS (30U * 60U * 1000U)
#define LOCATION_HS_TRACK 8

typedef struct {
    uint32_t addr;
    uint32_t next_attempt_ms;
    uint32_t backoff_ms;
    bool used;
} location_hs_slot_t;

typedef struct {
    location_hs_slot_t slots[LOCATION_HS_TRACK];
} location_hs_table_t;

void location_hs_reset(location_hs_table_t* table);

/* True when a handshake to addr may be attempted now, recording the attempt and
 * growing the backoff. Wrap-safe against the mesh clock. */
bool location_hs_should_attempt(location_hs_table_t* table, uint32_t addr, uint32_t now_ms);

/* Forget a peer's backoff, so its next need for a session attempts at once.
 * Called when a session to that peer reaches ACTIVE. */
void location_hs_clear(location_hs_table_t* table, uint32_t addr);

/* Longest channel-target NVS key plus terminator ("lch_" + two digits). */
#define LOCATION_TARGET_KEY_SIZE 16

/*
 * Retry delay after a share attempt that never reached the radio.
 *
 * Applied to CHANNEL targets only, and never stretched past the target's own
 * interval. A channel share fails when the TX gate rejects it (listen-before-
 * talk found the channel busy, or the airtime budget is spent), which is a
 * transient radio condition that clears on its own, costs one gate evaluation
 * to retest, and needs no state from any peer. Waiting a full share interval
 * to retry it throws away a position that is still fresh.
 *
 * CONTACT targets deliberately do not get this: their dominant failure is
 * "no ACTIVE DM session", which cannot resolve on a ten-second timescale
 * because it needs a handshake driven by other traffic. Retrying that fast
 * is a warning-logging busy loop that changes nothing, so a failed contact
 * share consumes its interval like a successful one.
 */
#define LOCATION_SEND_RETRY_S 10

/* One persisted target rule: whether it is active, at what resolution, and
 * how often. */
typedef struct {
    bool enabled;
    uint8_t tier; /* LOCATION_TIER_* */
    uint16_t interval_s;
} location_rule_t;

/* One resolved, enabled target for a share round. */
typedef struct {
    uint8_t kind; /* LOCATION_TARGET_* */
    uint32_t id;  /* peer address (CONTACT) or channel index (CHANNEL) */
    uint8_t tier; /* resolution to send at */
    uint16_t interval_s;
} location_target_t;

/*
 * Rule string codec, shared by the RPC config surface that writes these keys
 * and the send path that reads them, so the two cannot drift.
 *
 * Canonical form is "<enabled>|<tier>|<interval_s>", for example
 * "1|coarse|300". A bare tier name is also accepted and read as an enabled
 * rule at the default interval. Parsing always fills the rule (it has no
 * failure mode that leaves it unset) and returns false only for null
 * arguments.
 */
bool location_rule_parse(const char* raw, location_rule_t* rule);
void location_rule_format(char* out, size_t out_len, const location_rule_t* rule);

/* Build the canonical NVS key for a target. Returns false if the identifier
 * is out of range, which is how an out-of-range channel index is rejected
 * before it can be written as a key nothing will ever match. */
bool location_contact_key(char* out, size_t out_len, uint32_t addr);
bool location_channel_key(char* out, size_t out_len, int channel_index);

/*
 * Resolve one persisted NVS key/value pair into a share target.
 *
 * This is the whole target-selection decision: which keys name a target,
 * which kind each one is, and what tier and interval it carries. Returns
 * true only for a key that names a target whose rule is enabled.
 */
bool location_target_from_entry(const char* key, const char* raw, location_target_t* out);

/* Shortest interval among the given targets, in seconds, or 0 for none. This
 * is the node's effective share cadence, which is what the GNSS duty cycler
 * has to schedule against. */
uint16_t location_targets_min_interval_s(const location_target_t* targets, size_t count);

/* Should a share round run at all: sharing on, a position to send, and at
 * least one target to send it to. Interval pacing is per target and belongs
 * to the schedule below, not here. */
bool location_share_round_enabled(const location_policy_t* policy, bool has_source,
                                  size_t target_count);

/*
 * Per-target send schedule.
 *
 * Each target paces itself off its own interval, so a channel target set to
 * 60s and a contact rule set to 900s both get what they asked for. Slots are
 * keyed by kind and id, hold the time of the last ATTEMPT and how long that
 * attempt bought, and are compared with wrap-safe unsigned subtraction
 * because the mesh clock wraps. A target with no slot yet is due
 * immediately, so a freshly configured target shares on the next round
 * rather than after one silent interval.
 */
typedef struct {
    uint8_t kind;
    uint32_t id;
    uint32_t last_attempt_ms;
    uint32_t wait_ms;
    bool used;
} location_schedule_slot_t;

typedef struct {
    location_schedule_slot_t slots[LOCATION_MAX_TARGETS];
} location_schedule_t;

void location_schedule_init(location_schedule_t* sched);
bool location_schedule_is_due(const location_schedule_t* sched, const location_target_t* target,
                              uint32_t now_ms);
/* Record an attempt. `sent` false means the frame never reached the radio;
 * see LOCATION_SEND_RETRY_S for why that backs off differently per kind. */
void location_schedule_record(location_schedule_t* sched, const location_target_t* target,
                              uint32_t now_ms, bool sent);
/* Drop slots for targets that are no longer configured, so removing a rule
 * releases its slot instead of pinning it until the table wraps. */
void location_schedule_retain(location_schedule_t* sched, const location_target_t* targets,
                              size_t count);

/* Persistent policy helpers */
void location_policy_set_defaults(location_policy_t* policy);
void location_policy_normalize(location_policy_t* policy);
uint16_t location_policy_clamp_interval_s(uint16_t interval_s);
uint8_t location_tier_from_string(const char* tier);
const char* location_tier_to_string(uint8_t tier);

#endif
