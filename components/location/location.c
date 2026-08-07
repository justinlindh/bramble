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

int location_cache_update(location_manager_t* mgr, uint32_t peer_addr,
                          const bramble_position_t* pos, uint32_t now_ms) {
    /* Update existing */
    for (int i = 0; i < mgr->cache_count; i++) {
        if (mgr->cache[i].peer_addr == peer_addr) {
            mgr->cache[i].pos = *pos;
            mgr->cache[i].received_ms = now_ms;
            mgr->cache[i].active = true;
            return 0;
        }
    }
    /* Add new */
    if (mgr->cache_count >= LOCATION_MAX_CONTACTS)
        return -1;
    location_cache_entry_t* e = &mgr->cache[mgr->cache_count++];
    e->peer_addr = peer_addr;
    e->pos = *pos;
    e->received_ms = now_ms;
    e->active = true;
    return 0;
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
        if (now_ms - mgr->cache[i].received_ms > LOCATION_CACHE_TTL_MS) {
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

void location_hs_reset(location_hs_table_t* table) {
    if (table)
        memset(table, 0, sizeof(*table));
}

void location_hs_clear(location_hs_table_t* table, uint32_t addr) {
    if (!table)
        return;
    for (int i = 0; i < LOCATION_HS_TRACK; i++) {
        if (table->slots[i].used && table->slots[i].addr == addr) {
            memset(&table->slots[i], 0, sizeof(table->slots[i]));
            return;
        }
    }
}

bool location_hs_should_attempt(location_hs_table_t* table, uint32_t addr, uint32_t now_ms) {
    if (!table || addr == 0)
        return false;

    int free_idx = -1;
    int oldest = 0;
    for (int i = 0; i < LOCATION_HS_TRACK; i++) {
        location_hs_slot_t* slot = &table->slots[i];
        if (slot->used && slot->addr == addr) {
            /* Signed difference so the comparison survives the mesh clock
             * wrapping between the scheduled attempt and now. */
            if ((int32_t)(now_ms - slot->next_attempt_ms) < 0)
                return false;
            slot->backoff_ms = (slot->backoff_ms >= LOCATION_HS_BACKOFF_MAX_MS)
                                   ? LOCATION_HS_BACKOFF_MAX_MS
                                   : slot->backoff_ms * 2;
            if (slot->backoff_ms > LOCATION_HS_BACKOFF_MAX_MS)
                slot->backoff_ms = LOCATION_HS_BACKOFF_MAX_MS;
            slot->next_attempt_ms = now_ms + slot->backoff_ms;
            return true;
        }
        if (!slot->used && free_idx < 0)
            free_idx = i;
        if (table->slots[i].next_attempt_ms < table->slots[oldest].next_attempt_ms)
            oldest = i;
    }

    /* First time this peer needed a session: attempt immediately, and start the
     * backoff so a peer that never answers decays instead of retrying forever
     * at the share interval. */
    int idx = (free_idx >= 0) ? free_idx : oldest;
    table->slots[idx].addr = addr;
    table->slots[idx].used = true;
    table->slots[idx].backoff_ms = LOCATION_HS_BACKOFF_START_MS;
    table->slots[idx].next_attempt_ms = now_ms + LOCATION_HS_BACKOFF_START_MS;
    return true;
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
        /* Defence in depth: a public-channel rule persisted by an older build,
         * or written by any path that did not go through the RPC setter, must
         * not resolve to a target here either. */
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
