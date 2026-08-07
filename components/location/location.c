#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "location.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

loc_share_mode_t location_share_mode_get(void) {
    nvs_handle_t nvs;
    uint8_t mode = LOC_SHARE_OFF;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "loc_share", &mode);
        nvs_close(nvs);
    }
    if (mode >= LOC_SHARE_COUNT)
        mode = LOC_SHARE_OFF;
    return (loc_share_mode_t)mode;
}

void location_share_mode_set(loc_share_mode_t mode) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "loc_share", (uint8_t)mode);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void location_init(location_manager_t* mgr) { memset(mgr, 0, sizeof(*mgr)); }

void location_set_position(location_manager_t* mgr, const bramble_position_t* pos) {
    mgr->my_position = *pos;
}

/* Little-endian helpers */
static void loc_put_u32(uint8_t* buf, uint32_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF;
    buf[3] = (v >> 24) & 0xFF;
}
static uint32_t loc_get_u32(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}
static void loc_put_i32(uint8_t* buf, int32_t v) { loc_put_u32(buf, (uint32_t)v); }
static int32_t loc_get_i32(const uint8_t* buf) { return (int32_t)loc_get_u32(buf); }
static void loc_put_i16(uint8_t* buf, int16_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
}
static int16_t loc_get_i16(const uint8_t* buf) { return (int16_t)(buf[0] | (buf[1] << 8)); }

int location_serialize_full(const bramble_position_t* pos, uint8_t* buf, size_t buf_len) {
    if (buf_len < LOCATION_FULL_SIZE)
        return -1;
    loc_put_i32(buf + 0, pos->latitude_e7);
    loc_put_i32(buf + 4, pos->longitude_e7);
    loc_put_i16(buf + 8, pos->altitude_m);
    buf[10] = pos->accuracy_m;
    buf[11] = pos->speed_kmh;
    buf[12] = pos->heading_deg2;
    loc_put_u32(buf + 13, pos->timestamp);
    return LOCATION_FULL_SIZE;
}

int location_deserialize_full(const uint8_t* buf, size_t len, bramble_position_t* pos) {
    if (len < LOCATION_FULL_SIZE)
        return -1;
    pos->latitude_e7 = loc_get_i32(buf + 0);
    pos->longitude_e7 = loc_get_i32(buf + 4);
    pos->altitude_m = loc_get_i16(buf + 8);
    pos->accuracy_m = buf[10];
    pos->speed_kmh = buf[11];
    pos->heading_deg2 = buf[12];
    pos->timestamp = loc_get_u32(buf + 13);
    pos->valid = true;
    return LOCATION_FULL_SIZE;
}

/* Coarse: quantize to ~1km grid by dividing e7 by 10000.
 * Values are stored as int32 packed into 2 bytes each via modular truncation
 * for lat (range ±90000) and lon (range ±180000), use 4 bytes total.
 * Actually we split into high/low to fit the 5-byte format. */
int location_serialize_coarse(const bramble_position_t* pos, uint8_t* buf, size_t buf_len) {
    if (buf_len < LOCATION_COARSE_SIZE)
        return -1;
    /* Grid values: lat/10000 range is ±90000, lon/10000 range is ±180000.
     * These don't fit int16. Use offset encoding:
     * Store as unsigned by adding offset, then pack into 2 bytes.
     * lat: add 90000 -> 0..180000 -> divide by 3 -> 0..60000 fits uint16
     * lon: add 180000 -> 0..360000 -> divide by 6 -> 0..60000 fits uint16
     * This gives ~3.3km lat resolution, ~6.6km lon resolution. */
    int32_t grid_lat = pos->latitude_e7 / 10000;  /* ±90000 */
    int32_t grid_lon = pos->longitude_e7 / 10000; /* ±180000 */
    uint16_t enc_lat = (uint16_t)((grid_lat + 90000) / 3);
    uint16_t enc_lon = (uint16_t)((grid_lon + 180000) / 6);
    buf[0] = enc_lat & 0xFF;
    buf[1] = (enc_lat >> 8) & 0xFF;
    buf[2] = enc_lon & 0xFF;
    buf[3] = (enc_lon >> 8) & 0xFF;
    buf[4] = (uint8_t)(pos->timestamp & 0xFF);
    return LOCATION_COARSE_SIZE;
}

int location_deserialize_coarse(const uint8_t* buf, size_t len, bramble_position_t* pos) {
    if (len < LOCATION_COARSE_SIZE)
        return -1;
    memset(pos, 0, sizeof(*pos));
    uint16_t enc_lat = buf[0] | ((uint16_t)buf[1] << 8);
    uint16_t enc_lon = buf[2] | ((uint16_t)buf[3] << 8);
    int32_t grid_lat = (int32_t)enc_lat * 3 - 90000;
    int32_t grid_lon = (int32_t)enc_lon * 6 - 180000;
    pos->latitude_e7 = grid_lat * 10000;
    pos->longitude_e7 = grid_lon * 10000;
    pos->timestamp = buf[4]; /* only low byte */
    pos->valid = true;
    return LOCATION_COARSE_SIZE;
}

int location_serialize_presence(const bramble_position_t* pos, uint8_t* buf, size_t buf_len) {
    if (buf_len < LOCATION_PRESENCE_SIZE)
        return -1;
    buf[0] = (pos && pos->valid) ? 1 : 0;
    return LOCATION_PRESENCE_SIZE;
}

int location_serialize_for_tier(const bramble_position_t* pos, uint8_t tier, uint8_t* buf,
                                size_t buf_len) {
    switch (tier) {
    case LOCATION_TIER_FULL:
        return location_serialize_full(pos, buf, buf_len);
    case LOCATION_TIER_PRESENCE:
        return location_serialize_presence(pos, buf, buf_len);
    case LOCATION_TIER_COARSE:
    default:
        return location_serialize_coarse(pos, buf, buf_len);
    }
}

int location_deserialize_for_tier(const uint8_t* buf, size_t len, uint8_t tier,
                                  bramble_position_t* pos) {
    if (!buf || !pos)
        return -1;

    switch (tier) {
    case LOCATION_TIER_FULL:
        return location_deserialize_full(buf, len, pos);
    case LOCATION_TIER_PRESENCE:
        if (len < LOCATION_PRESENCE_SIZE)
            return -1;
        memset(pos, 0, sizeof(*pos));
        pos->valid = (buf[0] != 0);
        return LOCATION_PRESENCE_SIZE;
    case LOCATION_TIER_COARSE:
    default:
        return location_deserialize_coarse(buf, len, pos);
    }
}

int location_parse_inner(const uint8_t* plaintext, size_t plaintext_len, uint8_t* tier_out,
                         bramble_position_t* pos_out) {
    if (!plaintext || !tier_out || !pos_out || plaintext_len < 1)
        return -1;
    uint8_t tier = plaintext[LOCATION_INNER_TIER_OFFSET];
    /* Tier-appropriate deserialize length, ignoring the trailing canonical
     * L_LOC_INNER pad: PRESENCE/COARSE tiers only consume their own real
     * byte count even though the plaintext carries LOCATION_FULL_SIZE
     * bytes of position payload after the tier byte. */
    int n = location_deserialize_for_tier(plaintext + 1, plaintext_len - 1, tier, pos_out);
    if (n <= 0)
        return -1;
    *tier_out = tier;
    return 0;
}

/* The peer's slot, appending one if the peer is new. NULL when the cache is
 * full and the peer is not already in it. */
static location_cache_entry_t* cache_slot(location_manager_t* mgr, uint32_t peer_addr) {
    for (int i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].peer_addr == peer_addr)
            return &mgr->cache[i];
    }
    if (mgr->cache_count >= LOCATION_MAX_CONTACTS)
        return NULL;
    location_cache_entry_t* e = &mgr->cache[mgr->cache_count++];
    memset(e, 0, sizeof(*e));
    e->peer_addr = peer_addr;
    return e;
}

int location_cache_update(location_manager_t* mgr, uint32_t peer_addr,
                          const bramble_position_t* pos, uint32_t now_ms) {
    location_cache_entry_t* e = cache_slot(mgr, peer_addr);
    if (!e)
        return -1;
    e->pos = *pos;
    e->received_ms = now_ms;
    e->active = true;
    e->age_known = true;
    return 0;
}

void location_cache_drop(location_manager_t* mgr, uint32_t peer_addr) {
    for (int i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].peer_addr == peer_addr) {
            mgr->cache[i] = mgr->cache[--mgr->cache_count];
            return;
        }
    }
}

void location_cache_apply_share(location_manager_t* mgr, uint32_t peer_addr, uint8_t tier,
                                const bramble_position_t* pos, uint32_t now_ms) {
    if (!mgr || !pos)
        return;
    if (!location_tier_has_coordinates(tier) || !pos->valid) {
        location_cache_drop(mgr, peer_addr);
        return;
    }
    location_cache_update(mgr, peer_addr, pos, now_ms);
}

bool location_age_is_fresh(bool age_known, uint32_t received_ms, uint32_t now_ms) {
    if (!age_known)
        return false;
    return (uint32_t)(now_ms - received_ms) < LOCATION_CACHE_TTL_MS;
}

const location_cache_entry_t* location_cache_get(const location_manager_t* mgr,
                                                 uint32_t peer_addr) {
    for (int i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].peer_addr == peer_addr && mgr->cache[i].active)
            return &mgr->cache[i];
    }
    return NULL;
}

void location_cache_purge(location_manager_t* mgr, uint32_t now_ms) {
    for (int i = 0; i < mgr->cache_count;) {
        /* An entry restored from flash has no computable age, so there is no
         * expiry to test: subtracting a previous boot's uptime from this
         * one's yields a number with no meaning, and acting on it would evict
         * (or keep) at random. It stays as the peer's last known position
         * until a share received in THIS boot replaces it with a timestamp
         * the clock can actually measure. */
        if (mgr->cache[i].age_known && now_ms - mgr->cache[i].received_ms > LOCATION_CACHE_TTL_MS) {
            mgr->cache[i] = mgr->cache[--mgr->cache_count];
        } else {
            i++;
        }
    }
}

uint16_t location_policy_clamp_interval_s(uint16_t interval_s) {
    if (interval_s < LOCATION_MIN_INTERVAL_S) {
        return LOCATION_MIN_INTERVAL_S;
    }
    return interval_s;
}

uint8_t location_tier_from_string(const char* tier) {
    if (!tier)
        return LOCATION_TIER_COARSE;
    if (strcmp(tier, "full") == 0 || strcmp(tier, "exact") == 0)
        return LOCATION_TIER_FULL;
    if (strcmp(tier, "presence") == 0)
        return LOCATION_TIER_PRESENCE;
    return LOCATION_TIER_COARSE;
}

const char* location_tier_to_string(uint8_t tier) {
    switch (tier) {
    case LOCATION_TIER_FULL:
        return "full";
    case LOCATION_TIER_PRESENCE:
        return "presence";
    case LOCATION_TIER_COARSE:
    default:
        return "coarse";
    }
}

void location_policy_set_defaults(location_policy_t* policy) {
    if (!policy)
        return;
    policy->enabled = false;
    policy->default_tier = LOCATION_TIER_COARSE;
    policy->interval_s = LOCATION_DEFAULT_INTERVAL_S;
}

void location_policy_normalize(location_policy_t* policy) {
    if (!policy)
        return;
    if (policy->default_tier > LOCATION_TIER_PRESENCE) {
        policy->default_tier = LOCATION_TIER_COARSE;
    }
    policy->interval_s = location_policy_clamp_interval_s(policy->interval_s);
}

bool location_share_round_enabled(const location_policy_t* policy, bool has_source,
                                  size_t target_count) {
    if (!policy || !policy->enabled)
        return false;
    return has_source && target_count > 0;
}

bool location_rule_parse(const char* raw, location_rule_t* rule) {
    if (!raw || !rule)
        return false;

    int enabled = 1;
    char tier[16] = {0};
    int interval_s = LOCATION_DEFAULT_INTERVAL_S;
    int scanned = sscanf(raw, "%d|%15[^|]|%d", &enabled, tier, &interval_s);
    if (scanned >= 2) {
        rule->enabled = (enabled != 0);
        rule->tier = location_tier_from_string(tier);
        if (scanned >= 3 && interval_s > 0) {
            rule->interval_s = location_policy_clamp_interval_s((uint16_t)interval_s);
        } else {
            rule->interval_s = LOCATION_DEFAULT_INTERVAL_S;
        }
        return true;
    }

    rule->enabled = true;
    rule->tier = location_tier_from_string(raw);
    rule->interval_s = LOCATION_DEFAULT_INTERVAL_S;
    return true;
}

void location_rule_format(char* out, size_t out_len, const location_rule_t* rule) {
    if (!out || out_len == 0 || !rule)
        return;
    snprintf(out, out_len, "%d|%s|%u", rule->enabled ? 1 : 0, location_tier_to_string(rule->tier),
             (unsigned)rule->interval_s);
}

bool location_contact_key(char* out, size_t out_len, uint32_t addr) {
    if (!out || out_len < LOCATION_TARGET_KEY_SIZE)
        return false;
    snprintf(out, out_len, LOCATION_CONTACT_RULE_PREFIX "%08" PRIX32, addr);
    return true;
}

bool location_channel_target_is_permitted(int channel_index) {
    return channel_index != LOCATION_PUBLIC_CHANNEL_INDEX;
}

bool location_channel_key(char* out, size_t out_len, int channel_index) {
    if (!out || out_len < LOCATION_TARGET_KEY_SIZE)
        return false;
    /* Two digits is the whole key space this prefix has, so an index outside
     * it cannot be represented. Rejecting here is what stops a key nothing
     * will ever match from being written. */
    if (channel_index < 0 || channel_index >= LOCATION_MAX_CHANNEL_TARGETS)
        return false;
    snprintf(out, out_len, LOCATION_CHANNEL_RULE_PREFIX "%02d", channel_index);
    return true;
}

/* Exactly `digits` characters of the named base and then end of string. The key
 * builders above emit fixed-width suffixes (8 hex for a contact, 2 decimal for
 * a channel), so anything else is not a key this module wrote. strtoul/atoi
 * alone would not do: both accept a partial parse and report 0 for a suffix
 * with no digits at all, which would silently turn a foreign key into target 0
 * rather than rejecting it. */
static bool location_suffix_is_exact(const char* suffix, size_t digits, bool hex) {
    for (size_t i = 0; i < digits; i++) {
        char c = suffix[i];
        bool ok =
            (c >= '0' && c <= '9') || (hex && ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')));
        if (!ok)
            return false;
    }
    return suffix[digits] == '\0';
}

int location_channel_index_from_suffix(const char* suffix) {
    if (!suffix || !location_suffix_is_exact(suffix, 2, false))
        return -1;
    return atoi(suffix);
}

bool location_target_from_entry(const char* key, const char* raw, location_target_t* out) {
    if (!key || !raw || !out)
        return false;

    const size_t contact_prefix_len = sizeof(LOCATION_CONTACT_RULE_PREFIX) - 1;
    const size_t channel_prefix_len = sizeof(LOCATION_CHANNEL_RULE_PREFIX) - 1;

    uint8_t kind;
    uint32_t id;
    if (strncmp(key, LOCATION_CONTACT_RULE_PREFIX, contact_prefix_len) == 0) {
        const char* suffix = key + contact_prefix_len;
        if (!location_suffix_is_exact(suffix, 8, true))
            return false;
        kind = LOCATION_TARGET_CONTACT;
        id = (uint32_t)strtoul(suffix, NULL, 16);
    } else if (strncmp(key, LOCATION_CHANNEL_RULE_PREFIX, channel_prefix_len) == 0) {
        const char* suffix = key + channel_prefix_len;
        if (!location_suffix_is_exact(suffix, 2, false))
            return false;
        int index = atoi(suffix);
        if (index < 0 || index >= LOCATION_MAX_CHANNEL_TARGETS)
            return false;
        /* Defence in depth on the upgrade path: a public-channel rule written
         * by an earlier build, or by any path that did not go through the RPC
         * setter, must not resolve to a target here either, so upgrading
         * neutralises it rather than only preventing new ones. */
        if (!location_channel_target_is_permitted(index))
            return false;
        kind = LOCATION_TARGET_CHANNEL;
        id = (uint32_t)index;
    } else {
        return false;
    }

    location_rule_t rule = {
        .enabled = true,
        .tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };
    location_rule_parse(raw, &rule);
    if (!rule.enabled)
        return false;

    out->kind = kind;
    out->id = id;
    out->tier = rule.tier;
    out->interval_s = location_policy_clamp_interval_s(rule.interval_s);
    return true;
}

/* ── Persisted peer locations ───────────────────────────────────────────── */

/* The persisted boot counter's key, in NVS_NS_LOCATION beside the records it
 * stamps. Kept with the records rather than in a general namespace because
 * stamping them is the only thing it is for: a second consumer would want a
 * different bump point and would silently change what a stamp means. */
#define PEER_LOCATION_BOOT_ID_KEY "boot_id"

/* 0 until location_store_begin_boot runs (or if it could not reach flash),
 * which is also the value that makes every stored record decode as
 * age-unknown: fail closed, never claim a stale fix is current. */
static uint32_t s_boot_id;
static bool s_boot_id_resolved;

void peer_location_record_key(char* out, size_t out_len, uint32_t peer_addr) {
    if (!out || out_len == 0)
        return;
    snprintf(out, out_len, PEER_LOCATION_KEY_PREFIX "%08" PRIX32, peer_addr);
}

bool peer_location_key_parse(const char* key, uint32_t* peer_addr_out) {
    if (!key || !peer_addr_out)
        return false;
    if (strncmp(key, PEER_LOCATION_KEY_PREFIX, PEER_LOCATION_KEY_PREFIX_LEN) != 0)
        return false;
    const char* hex = key + PEER_LOCATION_KEY_PREFIX_LEN;
    if (*hex == '\0')
        return false;
    char* end = NULL;
    unsigned long addr = strtoul(hex, &end, 16);
    if (!end || *end != '\0')
        return false;
    *peer_addr_out = (uint32_t)addr;
    return true;
}

void peer_location_record_encode(persisted_peer_location_t* out, const bramble_position_t* pos,
                                 uint8_t tier, uint32_t now_ms, uint32_t boot_id) {
    if (!out || !pos)
        return;
    memset(out, 0, sizeof(*out));
    out->latitude_e7 = pos->latitude_e7;
    out->longitude_e7 = pos->longitude_e7;
    out->altitude_m = pos->altitude_m;
    out->accuracy_m = pos->accuracy_m;
    out->speed_kmh = pos->speed_kmh;
    out->heading_deg2 = pos->heading_deg2;
    out->timestamp = pos->timestamp;
    out->received_ms = now_ms;
    out->tier = tier;
    out->version = PEER_LOCATION_RECORD_VERSION;
    out->boot_id = boot_id;
}

int peer_location_record_decode(const void* blob, size_t len, uint32_t current_boot_id,
                                peer_location_record_t* out) {
    if (!blob || !out)
        return -1;

    persisted_peer_location_t rec;
    memset(&rec, 0, sizeof(rec));
    if (len == sizeof(rec)) {
        memcpy(&rec, blob, sizeof(rec));
        if (rec.version != PEER_LOCATION_RECORD_VERSION)
            return -1;
    } else if (len == PEER_LOCATION_RECORD_V0_SIZE) {
        /* Pre-boot-counter record: the new fields are a pure append, so the
         * old bytes parse as the leading prefix and the absent boot_id stays
         * 0, which is the reserved "unknown boot" value. */
        memcpy(&rec, blob, PEER_LOCATION_RECORD_V0_SIZE);
        rec.version = 0;
        rec.boot_id = 0;
    } else {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->pos.latitude_e7 = rec.latitude_e7;
    out->pos.longitude_e7 = rec.longitude_e7;
    out->pos.altitude_m = rec.altitude_m;
    out->pos.accuracy_m = rec.accuracy_m;
    out->pos.speed_kmh = rec.speed_kmh;
    out->pos.heading_deg2 = rec.heading_deg2;
    out->pos.timestamp = rec.timestamp;
    out->pos.valid = true;
    out->tier = rec.tier;
    out->received_ms = rec.received_ms;
    out->boot_id = rec.boot_id;
    out->age_known = (rec.boot_id != 0 && rec.boot_id == current_boot_id);
    return 0;
}

uint32_t location_store_begin_boot(void) {
    if (s_boot_id_resolved)
        return s_boot_id;
    s_boot_id_resolved = true;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
        return s_boot_id; /* stays 0: every record reads as age-unknown */
    }

    uint32_t stored = 0;
    nvs_get_u32(nvs, PEER_LOCATION_BOOT_ID_KEY, &stored);
    uint32_t next = stored + 1;
    if (next == 0)
        next = 1; /* wrap past the reserved "unknown" value */

    if (nvs_set_u32(nvs, PEER_LOCATION_BOOT_ID_KEY, next) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
        s_boot_id = next;
    }
    nvs_close(nvs);
    return s_boot_id;
}

uint32_t location_store_boot_id(void) { return s_boot_id; }

void location_store_reset_boot_state(void) {
    s_boot_id = 0;
    s_boot_id_resolved = false;
}

void location_store_save_peer(uint32_t peer_addr, const bramble_position_t* pos, uint8_t tier,
                              uint32_t now_ms) {
    if (!pos)
        return;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    char key[PEER_LOCATION_KEY_MAX];
    peer_location_record_key(key, sizeof(key), peer_addr);

    persisted_peer_location_t stored;
    peer_location_record_encode(&stored, pos, tier, now_ms, location_store_boot_id());

    nvs_set_blob(nvs, key, &stored, sizeof(stored));
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Insert into a bounded array kept sorted newest-first. "Newest" is
 * (boot_id, received_ms): a higher boot counter is a later boot, and within
 * one boot the uptime clock orders correctly. That makes the choice of which
 * records survive the cache's slot limit deterministic and sensible rather
 * than "whatever the NVS directory happened to list first". */
static void restore_insert(peer_location_restore_entry_t* out, int* count, int max,
                           const peer_location_restore_entry_t* candidate) {
    int pos = *count;
    for (int i = 0; i < *count; i++) {
        bool newer = (candidate->rec.boot_id > out[i].rec.boot_id) ||
                     (candidate->rec.boot_id == out[i].rec.boot_id &&
                      candidate->rec.received_ms > out[i].rec.received_ms);
        if (newer) {
            pos = i;
            break;
        }
    }
    if (pos >= max)
        return; /* older than everything already kept, and no room left */

    if (*count < max)
        (*count)++;
    for (int i = *count - 1; i > pos; i--) {
        out[i] = out[i - 1];
    }
    out[pos] = *candidate;
}

int location_store_collect(peer_location_restore_entry_t* out, int max) {
    if (!out || max <= 0)
        return 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
        return 0;
    }

    uint32_t boot_id = location_store_boot_id();
    int count = 0;

    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) == ESP_OK) {
        while (it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            peer_location_restore_entry_t candidate;
            if (peer_location_key_parse(info.key, &candidate.peer_addr)) {
                persisted_peer_location_t blob;
                size_t len = sizeof(blob);
                if (nvs_get_blob(nvs, info.key, &blob, &len) == ESP_OK &&
                    peer_location_record_decode(&blob, len, boot_id, &candidate.rec) == 0) {
                    restore_insert(out, &count, max, &candidate);
                }
            }

            if (nvs_entry_next(&it) != ESP_OK) {
                break;
            }
        }
        nvs_release_iterator(it);
    }

    nvs_close(nvs);
    return count;
}

void location_store_apply(location_manager_t* mgr, const peer_location_restore_entry_t* entries,
                          int count) {
    if (!mgr || !entries)
        return;
    for (int i = 0; i < count; i++) {
        const peer_location_record_t* rec = &entries[i].rec;
        /* Same tier rule the live RX path applies: only a coordinate-bearing
         * record can be placed, and a peer whose last share was PRESENCE has
         * no position to restore. */
        if (!location_tier_has_coordinates(rec->tier) || !rec->pos.valid)
            continue;
        location_cache_entry_t* e = cache_slot(mgr, entries[i].peer_addr);
        if (!e)
            break; /* cache full, and the remaining entries are older */
        e->pos = rec->pos;
        e->received_ms = rec->received_ms;
        e->active = true;
        e->age_known = rec->age_known;
    }
}

uint16_t location_targets_min_interval_s(const location_target_t* targets, size_t count) {
    if (!targets || count == 0)
        return 0;
    uint16_t min_s = 0;
    for (size_t i = 0; i < count; i++) {
        if (min_s == 0 || targets[i].interval_s < min_s)
            min_s = targets[i].interval_s;
    }
    return min_s;
}

void location_schedule_init(location_schedule_t* sched) {
    if (!sched)
        return;
    memset(sched, 0, sizeof(*sched));
}

static location_schedule_slot_t* location_schedule_find(location_schedule_t* sched, uint8_t kind,
                                                        uint32_t id) {
    for (size_t i = 0; i < LOCATION_MAX_TARGETS; i++) {
        if (sched->slots[i].used && sched->slots[i].kind == kind && sched->slots[i].id == id)
            return &sched->slots[i];
    }
    return NULL;
}

bool location_schedule_is_due(const location_schedule_t* sched, const location_target_t* target,
                              uint32_t now_ms) {
    if (!sched || !target)
        return false;
    const location_schedule_slot_t* slot =
        location_schedule_find((location_schedule_t*)sched, target->kind, target->id);
    if (!slot)
        return true; /* never attempted: share on this round */
    return (uint32_t)(now_ms - slot->last_attempt_ms) >= slot->wait_ms;
}

void location_schedule_record(location_schedule_t* sched, const location_target_t* target,
                              uint32_t now_ms, bool sent) {
    if (!sched || !target)
        return;

    uint32_t interval_ms = (uint32_t)location_policy_clamp_interval_s(target->interval_s) * 1000U;
    uint32_t wait_ms = interval_ms;
    if (!sent && target->kind == LOCATION_TARGET_CHANNEL) {
        uint32_t retry_ms = (uint32_t)LOCATION_SEND_RETRY_S * 1000U;
        wait_ms = retry_ms < interval_ms ? retry_ms : interval_ms;
    }

    location_schedule_slot_t* slot = location_schedule_find(sched, target->kind, target->id);
    if (!slot) {
        for (size_t i = 0; i < LOCATION_MAX_TARGETS; i++) {
            if (!sched->slots[i].used) {
                slot = &sched->slots[i];
                break;
            }
        }
    }
    if (!slot) {
        /* Unreachable while the collector caps targets at LOCATION_MAX_TARGETS
         * and retain() releases departed slots, but a full table must never
         * silently un-pace a target: take the slot that has waited longest. */
        slot = &sched->slots[0];
        for (size_t i = 1; i < LOCATION_MAX_TARGETS; i++) {
            if ((uint32_t)(now_ms - sched->slots[i].last_attempt_ms) >
                (uint32_t)(now_ms - slot->last_attempt_ms)) {
                slot = &sched->slots[i];
            }
        }
    }

    slot->used = true;
    slot->kind = target->kind;
    slot->id = target->id;
    slot->last_attempt_ms = now_ms;
    slot->wait_ms = wait_ms;
}

void location_schedule_retain(location_schedule_t* sched, const location_target_t* targets,
                              size_t count) {
    if (!sched)
        return;
    for (size_t i = 0; i < LOCATION_MAX_TARGETS; i++) {
        if (!sched->slots[i].used)
            continue;
        bool still_configured = false;
        for (size_t j = 0; targets && j < count; j++) {
            if (targets[j].kind == sched->slots[i].kind && targets[j].id == sched->slots[i].id) {
                still_configured = true;
                break;
            }
        }
        if (!still_configured) {
            sched->slots[i].used = false;
        }
    }
}
