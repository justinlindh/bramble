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
#define LOCATION_DEFAULT_INTERVAL_MS 300000 /* 5 minutes */
#define LOCATION_DEFAULT_INTERVAL_S 300     /* 5 minutes */
#define LOCATION_MIN_INTERVAL_S 30          /* floor for periodic sharing */
#define LOCATION_MIN_DISTANCE_M 100         /* distance trigger */
#define LOCATION_CACHE_TTL_MS 3600000       /* 1 hour */

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

/* Contact sharing config */
typedef struct {
    uint32_t peer_addr;
    uint8_t tier; /* LOCATION_TIER_* */
    bool active;
    bool auto_approve_requests;
    uint32_t last_sent_ms;
} location_contact_t;

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
    location_contact_t contacts[LOCATION_MAX_CONTACTS];
    int contact_count;
    location_cache_entry_t cache[LOCATION_MAX_CONTACTS];
    int cache_count;
    uint32_t update_interval_ms;
    uint16_t min_distance_m;
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

/* Contact management */
int location_add_contact(location_manager_t* mgr, uint32_t peer_addr, uint8_t tier);
int location_remove_contact(location_manager_t* mgr, uint32_t peer_addr);
location_contact_t* location_find_contact(location_manager_t* mgr, uint32_t peer_addr);

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

/* Check if update needed (time or distance based) */
bool location_should_send(const location_manager_t* mgr, uint32_t peer_addr, uint32_t now_ms);

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
