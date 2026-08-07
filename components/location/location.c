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

bool location_policy_should_send(const location_policy_t* policy, bool has_source, bool has_targets,
                                 uint32_t now_ms, uint32_t last_sent_ms) {
    if (!policy || !policy->enabled)
        return false;
    if (!has_source || !has_targets)
        return false;

    uint32_t interval_ms = (uint32_t)location_policy_clamp_interval_s(policy->interval_s) * 1000U;
    if (last_sent_ms != 0 && (now_ms - last_sent_ms) < interval_ms) {
        return false;
    }

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

    if (nvs_set_u32(nvs, PEER_LOCATION_BOOT_ID_KEY, next) == ESP_OK &&
        nvs_commit(nvs) == ESP_OK) {
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
