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

/*
 * Cached position from a peer.
 *
 * INVARIANT: every active entry holds coordinates that can actually be
 * PLACED on a map. A PRESENCE-tier share carries no coordinates at all (its
 * position struct is zeroed with valid set, meaning "the peer is there", not
 * "the peer is at 0,0"), so it never enters the cache; see
 * location_cache_apply_share.
 *
 * age_known says whether received_ms can be compared against the current
 * uptime clock. It is true for a share received during this boot and false
 * for an entry restored from flash, whose received_ms was written against a
 * clock that no longer exists. See the persisted-record comment below.
 */
typedef struct {
    uint32_t peer_addr;
    bramble_position_t pos;
    uint32_t received_ms;
    bool active;
    bool age_known;
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

/*
 * A tier that actually carries coordinates. PRESENCE carries a single
 * online/offline bit, and its deserialize leaves an all-zero position with
 * valid set, so anything that PLACES a peer must gate on this or a presence
 * share plants a marker on Null Island.
 */
static inline bool location_tier_has_coordinates(uint8_t tier) {
    return tier == LOCATION_TIER_FULL || tier == LOCATION_TIER_COARSE;
}

/* Cache */
int location_cache_update(location_manager_t* mgr, uint32_t peer_addr,
                          const bramble_position_t* pos, uint32_t now_ms);
const location_cache_entry_t* location_cache_get(const location_manager_t* mgr, uint32_t peer_addr);
void location_cache_purge(location_manager_t* mgr, uint32_t now_ms);

/* Forget everything cached for one peer. */
void location_cache_drop(location_manager_t* mgr, uint32_t peer_addr);

/*
 * Apply a decoded, authenticated location share to the cache with the tier's
 * own semantics. Every RX path goes through here (firmware handle_location
 * and the simulator's bridge) so the tier rules cannot drift between them:
 *
 *   - a coordinate-bearing tier updates the peer's position;
 *   - PRESENCE DROPS whatever coordinates the cache still holds for that
 *     peer. The sender chose to stop sharing a position, and continuing to
 *     show the last exact one they sent would leak precisely what they just
 *     withdrew.
 */
void location_cache_apply_share(location_manager_t* mgr, uint32_t peer_addr, uint8_t tier,
                                const bramble_position_t* pos, uint32_t now_ms);

/*
 * Is a cached or persisted position recent enough to present as current?
 * An entry whose age is not known is never fresh: the node cannot tell
 * whether it is a minute or a month old, and guessing "current" is the
 * failure mode this rule exists to prevent.
 */
bool location_age_is_fresh(bool age_known, uint32_t received_ms, uint32_t now_ms);

/* ── Persisted peer locations ────────────────────────────────────────────
 *
 * On-flash record for a peer's last known position: NVS namespace
 * NVS_NS_LOCATION, key PEER_LOCATION_KEY_PREFIX followed by the peer address
 * as 8 uppercase hex digits. ONE definition of the layout, deliberately: it
 * used to be hand-copied into three translation units, and a hand-maintained
 * second copy of a flash layout drifts and then mis-reads flash in silence.
 *
 * received_ms is a raw uptime millisecond count, so it only means something
 * inside the boot that wrote it. The counter restarts at zero on every reset,
 * which is why a stored value can read LARGER than the node's current uptime:
 * that is not a timestamp from the future, it is a reading from a clock that
 * no longer exists, and any freshness or staleness arithmetic against the
 * current clock is wrong across a restart. There is nothing better to store
 * instead: the node has no RTC and no time sync, and a position's own
 * timestamp field is the SENDER's uptime seconds, not epoch seconds. No value
 * written to flash can make a cross-boot age computable.
 *
 * What IS knowable is which boot produced the reading. boot_id carries the
 * persistent boot counter's value at write time, and an age is computable
 * only when it matches the current boot's. Otherwise the age is UNKNOWN, and
 * unknown is reported as unknown rather than guessed: the position is still
 * the best last-known fix the node has, and it is still worth showing, but
 * never as a current one.
 */
#define PEER_LOCATION_KEY_PREFIX "lp_"
#define PEER_LOCATION_KEY_PREFIX_LEN 3
#define PEER_LOCATION_KEY_MAX 16 /* "lp_" + 8 hex digits + NUL */

#define PEER_LOCATION_RECORD_VERSION 1

typedef struct __attribute__((packed)) {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2;
    uint32_t timestamp;
    uint32_t received_ms;
    uint8_t tier;
    uint8_t version; /* PEER_LOCATION_RECORD_VERSION */
    uint32_t boot_id;
} persisted_peer_location_t;

/*
 * Byte length of the original layout, which had neither version nor boot_id.
 * Records that size are still on flash on every node that has run an earlier
 * build, and the new fields are a pure append, so they decode as "age
 * unknown", which is exactly what they are.
 */
#define PEER_LOCATION_RECORD_V0_SIZE 22u

/* Decoded view of one on-flash record. */
typedef struct {
    bramble_position_t pos;
    uint8_t tier;
    uint32_t received_ms;
    uint32_t boot_id; /* 0 when the record predates the boot counter */
    bool age_known;   /* received_ms is comparable to the current uptime */
} peer_location_record_t;

/* Format the NVS key for a peer's record. */
void peer_location_record_key(char* out, size_t out_len, uint32_t peer_addr);

/* Parse the peer address back out of such a key. Returns false if it is not
 * a peer-location key. */
bool peer_location_key_parse(const char* key, uint32_t* peer_addr_out);

/* Fill an on-flash record. boot_id is the current boot counter. */
void peer_location_record_encode(persisted_peer_location_t* out, const bramble_position_t* pos,
                                 uint8_t tier, uint32_t now_ms, uint32_t boot_id);

/* Decode a blob of either layout. Returns 0 on success, -1 if the blob is not
 * a record this build understands. */
int peer_location_record_decode(const void* blob, size_t len, uint32_t current_boot_id,
                                peer_location_record_t* out);

/* One collected record on its way from flash into the in-RAM cache. */
typedef struct {
    uint32_t peer_addr;
    peer_location_record_t rec;
} peer_location_restore_entry_t;

/*
 * Boot counter, persisted alongside the records it stamps. Bumps once per
 * boot and returns the new value; calling it again in the same boot returns
 * the same value without touching flash. Zero is reserved to mean "no boot
 * counter", so the very first bump yields 1 and a record stamped 0 can never
 * be mistaken for a current-boot record.
 */
uint32_t location_store_begin_boot(void);

/* The current boot counter, or 0 if location_store_begin_boot has not run or
 * could not reach flash. */
uint32_t location_store_boot_id(void);

/*
 * Forget the cached boot counter so the next location_store_begin_boot reads
 * flash again. A device gets this from the reset vector; host tests that
 * simulate a reboot (module state cleared, flash contents kept) need to ask
 * for it.
 */
void location_store_reset_boot_state(void);

/* Persist one peer's position. */
void location_store_save_peer(uint32_t peer_addr, const bramble_position_t* pos, uint8_t tier,
                              uint32_t now_ms);

/*
 * Boot-time restore, phase 1 of 2: read flash into `out`, newest first, and
 * touch nothing else. Returns how many entries were filled, at most `max`.
 *
 * Split from the apply phase because the NVS iterator holds the shim's mutex
 * for the whole iteration, and that mutex must stay strictly BELOW every
 * other lock in the system (see the two documented reverse orderings in
 * mesh_send_location_updates). This phase therefore runs holding nothing, and
 * the caller only takes its own state lock afterwards, for the pure-RAM apply
 * below.
 *
 * Flash can hold more records than the cache has slots, so ordering matters:
 * entries come back sorted by (boot_id, received_ms) descending, which puts
 * the most recently written records in the surviving slots.
 */
int location_store_collect(peer_location_restore_entry_t* out, int max);

/*
 * Boot-time restore, phase 2 of 2: seed the cache from collected records.
 * Pure RAM, no NVS, so the caller may hold the lock that protects `mgr`.
 * Restored entries are marked age-unknown.
 */
void location_store_apply(location_manager_t* mgr, const peer_location_restore_entry_t* entries,
                          int count);

/* Policy engine send gating for periodic sharing */
bool location_policy_should_send(const location_policy_t* policy, bool has_source, bool has_targets,
                                 uint32_t now_ms, uint32_t last_sent_ms);

/* Persistent policy helpers */
void location_policy_set_defaults(location_policy_t* policy);
void location_policy_normalize(location_policy_t* policy);
uint16_t location_policy_clamp_interval_s(uint16_t interval_s);
uint8_t location_tier_from_string(const char* tier);
const char* location_tier_to_string(uint8_t tier);

#endif
