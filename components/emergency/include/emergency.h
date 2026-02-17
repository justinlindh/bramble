#ifndef BRAMBLE_EMERGENCY_H
#define BRAMBLE_EMERGENCY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Emergency states */
#define EMERGENCY_STATE_INACTIVE  0
#define EMERGENCY_STATE_ACTIVE    1
#define EMERGENCY_STATE_COOLDOWN  2

/* Timing */
#define EMERGENCY_AUTO_TIMEOUT_MS   (24ULL * 3600 * 1000)  /* 24h */
#define EMERGENCY_COOLDOWN_MS       (15 * 60 * 1000)       /* 15 min post-cancel */
#define EMERGENCY_BEACON_INTERVAL_MS 30000                  /* 30s between beacons */
#define EMERGENCY_MIN_ACTIVATION_MS  (3600 * 1000)         /* 1h between activations */
#define EMERGENCY_SHORT_MSG_LEN     32

/* Emergency beacon payload */
typedef struct {
    uint32_t src_addr;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
    uint8_t battery_pct;
    uint32_t timestamp;
    char short_msg[EMERGENCY_SHORT_MSG_LEN];
    uint8_t msg_len;
} emergency_beacon_t;

#define EMERGENCY_BEACON_MIN_SIZE 17  /* without short_msg */
#define EMERGENCY_BEACON_MAX_SIZE (17 + EMERGENCY_SHORT_MSG_LEN)

/* Emergency cancel payload — must be authenticated */
typedef struct {
    uint32_t src_addr;
    uint32_t cancel_timestamp;
    uint8_t auth_tag[4];  /* HMAC-SHA256 truncated to 4 bytes */
} emergency_cancel_t;

#define EMERGENCY_CANCEL_SIZE 12

/* Emergency manager state */
typedef struct {
    uint8_t state;
    uint32_t activated_at_ms;
    uint32_t last_beacon_ms;
    uint32_t last_activation_ms;  /* for rate limiting */
    emergency_beacon_t beacon;
    /* Tracking active emergencies from other nodes */
    struct {
        uint32_t src_addr;
        uint32_t received_ms;
        emergency_beacon_t beacon;
        bool active;
    } known_emergencies[8];
    int known_count;
} emergency_manager_t;

/* Init/lifecycle */
void emergency_init(emergency_manager_t *mgr);
int emergency_activate(emergency_manager_t *mgr, int32_t lat_e7, int32_t lon_e7,
                       int16_t alt_m, uint8_t battery, const char *msg, uint32_t now_ms);
int emergency_cancel(emergency_manager_t *mgr, uint32_t now_ms);
bool emergency_is_active(const emergency_manager_t *mgr);

/* Tick — called periodically, handles auto-timeout and cooldown transitions */
void emergency_tick(emergency_manager_t *mgr, uint32_t now_ms);

/* Should we send a beacon now? */
bool emergency_should_beacon(const emergency_manager_t *mgr, uint32_t now_ms);

/* Serialize/deserialize */
int emergency_beacon_serialize(const emergency_beacon_t *beacon, uint8_t *buf, size_t buf_len);
int emergency_beacon_deserialize(const uint8_t *buf, size_t len, emergency_beacon_t *beacon);
int emergency_cancel_serialize(const emergency_cancel_t *cancel, uint8_t *buf, size_t buf_len);
int emergency_cancel_deserialize(const uint8_t *buf, size_t len, emergency_cancel_t *cancel);

/* Track emergencies from other nodes */
int emergency_record_received(emergency_manager_t *mgr, const emergency_beacon_t *beacon, uint32_t now_ms);
int emergency_record_cancel(emergency_manager_t *mgr, uint32_t src_addr);
int emergency_get_active_count(const emergency_manager_t *mgr);

#endif
