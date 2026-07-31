#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "location.h"
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
