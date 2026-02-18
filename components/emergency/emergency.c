#include "emergency.h"
#include <string.h>

/* Little-endian helpers */
static void emg_put_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

static uint32_t emg_get_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static void emg_put_i32(uint8_t *buf, int32_t v) { emg_put_u32(buf, (uint32_t)v); }
static int32_t emg_get_i32(const uint8_t *buf) { return (int32_t)emg_get_u32(buf); }

static void emg_put_i16(uint8_t *buf, int16_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
}

static int16_t emg_get_i16(const uint8_t *buf) {
    return (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

void emergency_init(emergency_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->state = EMERGENCY_STATE_INACTIVE;
}

int emergency_activate(emergency_manager_t *mgr, int32_t lat_e7, int32_t lon_e7,
                       int16_t alt_m, uint8_t battery, const char *msg, uint32_t now_ms) {
    if (mgr->state != EMERGENCY_STATE_INACTIVE) {
        return -1;
    }

    /* Rate limit: 1h between activations */
    if (mgr->last_activation_ms != 0 &&
        (now_ms - mgr->last_activation_ms) < EMERGENCY_MIN_ACTIVATION_MS) {
        return -2;
    }

    mgr->state = EMERGENCY_STATE_ACTIVE;
    mgr->activated_at_ms = now_ms;
    mgr->last_beacon_ms = 0;  /* send immediately */
    mgr->last_activation_ms = now_ms;

    mgr->beacon.latitude_e7 = lat_e7;
    mgr->beacon.longitude_e7 = lon_e7;
    mgr->beacon.altitude_m = alt_m;
    mgr->beacon.battery_pct = battery;
    mgr->beacon.timestamp = now_ms;

    if (msg && msg[0]) {
        size_t len = strlen(msg);
        if (len > EMERGENCY_SHORT_MSG_LEN) len = EMERGENCY_SHORT_MSG_LEN;
        memcpy(mgr->beacon.short_msg, msg, len);
        mgr->beacon.msg_len = (uint8_t)len;
    } else {
        mgr->beacon.msg_len = 0;
    }

    return 0;
}

int emergency_cancel(emergency_manager_t *mgr, uint32_t now_ms) {
    if (mgr->state != EMERGENCY_STATE_ACTIVE) {
        return -1;
    }
    mgr->state = EMERGENCY_STATE_COOLDOWN;
    mgr->activated_at_ms = now_ms;  /* reuse as cooldown start */
    return 0;
}

bool emergency_is_active(const emergency_manager_t *mgr) {
    return mgr->state == EMERGENCY_STATE_ACTIVE;
}

void emergency_tick(emergency_manager_t *mgr, uint32_t now_ms) {
    if (mgr->state == EMERGENCY_STATE_ACTIVE) {
        uint64_t elapsed = (uint64_t)(now_ms - mgr->activated_at_ms);
        if (elapsed >= EMERGENCY_AUTO_TIMEOUT_MS) {
            mgr->state = EMERGENCY_STATE_INACTIVE;
        }
    } else if (mgr->state == EMERGENCY_STATE_COOLDOWN) {
        if ((now_ms - mgr->activated_at_ms) >= EMERGENCY_COOLDOWN_MS) {
            mgr->state = EMERGENCY_STATE_INACTIVE;
        }
    }
}

bool emergency_should_beacon(const emergency_manager_t *mgr, uint32_t now_ms) {
    if (mgr->state != EMERGENCY_STATE_ACTIVE) return false;
    if (mgr->last_beacon_ms == 0) return true;
    return (now_ms - mgr->last_beacon_ms) >= EMERGENCY_BEACON_INTERVAL_MS;
}

/*
 * Beacon binary layout (little-endian):
 *  [0..3]   src_addr      (u32)
 *  [4..7]   latitude_e7   (i32)
 *  [8..11]  longitude_e7  (i32)
 *  [12..13] altitude_m    (i16)
 *  [14]     battery_pct   (u8)
 *  [15..16] msg_len (u8) + reserved (u8)
 *  [17..]   short_msg     (msg_len bytes)
 *
 * Note: timestamp omitted from wire format to save space (receiver uses rx time).
 * Total min = 17, max = 17 + 32 = 49
 */

int emergency_beacon_serialize(const emergency_beacon_t *beacon, uint8_t *buf, size_t buf_len) {
    size_t needed = EMERGENCY_BEACON_MIN_SIZE + beacon->msg_len;
    if (buf_len < needed) return -1;

    emg_put_u32(buf + 0, beacon->src_addr);
    emg_put_i32(buf + 4, beacon->latitude_e7);
    emg_put_i32(buf + 8, beacon->longitude_e7);
    emg_put_i16(buf + 12, beacon->altitude_m);
    buf[14] = beacon->battery_pct;
    buf[15] = beacon->msg_len;
    buf[16] = 0;  /* reserved */

    if (beacon->msg_len > 0) {
        memcpy(buf + 17, beacon->short_msg, beacon->msg_len);
    }

    return (int)needed;
}

int emergency_beacon_deserialize(const uint8_t *buf, size_t len, emergency_beacon_t *beacon) {
    if (len < EMERGENCY_BEACON_MIN_SIZE) return -1;

    memset(beacon, 0, sizeof(*beacon));
    beacon->src_addr = emg_get_u32(buf + 0);
    beacon->latitude_e7 = emg_get_i32(buf + 4);
    beacon->longitude_e7 = emg_get_i32(buf + 8);
    beacon->altitude_m = emg_get_i16(buf + 12);
    beacon->battery_pct = buf[14];
    beacon->msg_len = buf[15];

    if (beacon->msg_len > EMERGENCY_SHORT_MSG_LEN) return -1;
    if (len < (size_t)(EMERGENCY_BEACON_MIN_SIZE + beacon->msg_len)) return -1;

    if (beacon->msg_len > 0) {
        memcpy(beacon->short_msg, buf + 17, beacon->msg_len);
    }

    return 0;
}

int emergency_cancel_serialize(const emergency_cancel_t *cancel, uint8_t *buf, size_t buf_len) {
    if (buf_len < EMERGENCY_CANCEL_SIZE) return -1;

    emg_put_u32(buf + 0, cancel->src_addr);
    emg_put_u32(buf + 4, cancel->cancel_timestamp);
    memcpy(buf + 8, cancel->auth_tag, 8);

    return EMERGENCY_CANCEL_SIZE;
}

int emergency_cancel_deserialize(const uint8_t *buf, size_t len, emergency_cancel_t *cancel) {
    if (len < EMERGENCY_CANCEL_SIZE) return -1;

    cancel->src_addr = emg_get_u32(buf + 0);
    cancel->cancel_timestamp = emg_get_u32(buf + 4);
    memcpy(cancel->auth_tag, buf + 8, 8);

    return 0;
}

int emergency_record_received(emergency_manager_t *mgr, const emergency_beacon_t *beacon, uint32_t now_ms) {
    /* Check for duplicate / update */
    for (int i = 0; i < mgr->known_count; i++) {
        if (mgr->known_emergencies[i].src_addr == beacon->src_addr) {
            mgr->known_emergencies[i].beacon = *beacon;
            mgr->known_emergencies[i].received_ms = now_ms;
            mgr->known_emergencies[i].active = true;
            return 0;
        }
    }

    if (mgr->known_count >= 8) return -1;  /* table full */

    int idx = mgr->known_count++;
    mgr->known_emergencies[idx].src_addr = beacon->src_addr;
    mgr->known_emergencies[idx].received_ms = now_ms;
    mgr->known_emergencies[idx].beacon = *beacon;
    mgr->known_emergencies[idx].active = true;

    return 0;
}

int emergency_record_cancel(emergency_manager_t *mgr, uint32_t src_addr) {
    for (int i = 0; i < mgr->known_count; i++) {
        if (mgr->known_emergencies[i].src_addr == src_addr) {
            mgr->known_emergencies[i].active = false;
            return 0;
        }
    }
    return -1;  /* not found */
}

int emergency_get_active_count(const emergency_manager_t *mgr) {
    int count = 0;
    for (int i = 0; i < mgr->known_count; i++) {
        if (mgr->known_emergencies[i].active) count++;
    }
    return count;
}
