#include "include/beacon.h"
#include "crypto.h"
#include <string.h>

bramble_beacon_t beacon_build(uint32_t my_addr, uint32_t pubkey_hash, uint16_t uptime_min,
                              uint8_t battery_pct, uint8_t tx_queue_depth, uint8_t neighbor_count,
                              uint8_t flags, uint32_t network_time, uint16_t time_confidence) {
    bramble_beacon_t b;
    memset(&b, 0, sizeof(b));
    b.header.version = BRAMBLE_VERSION;
    b.header.type = PKT_TYPE_BEACON;
    b.header.flags = 0;
    b.header.hop_limit = 1;          /* beacons are single-hop */
    b.header.dest_addr = 0xFFFFFFFF; /* broadcast */
    b.header.packet_id = 0;          /* caller should set */
    b.src_addr = my_addr;
    b.pubkey_hash = pubkey_hash;
    b.uptime_min = uptime_min;
    b.battery_pct = battery_pct;
    b.tx_queue_depth = tx_queue_depth;
    b.neighbor_count = neighbor_count;
    b.flags = flags;
    b.network_time = network_time;
    b.time_confidence = time_confidence;
    return b;
}

void beacon_compute_hmac(bramble_beacon_t* beacon, const uint8_t* shared_key, size_t key_len) {
    /* Serialize beacon with zeroed HMAC, then compute over the pre-HMAC portion.
     * Buffer must be large enough for name extension (BEACON_SIZE + 1 + BEACON_NAME_MAX). */
    uint8_t buf[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    memset(beacon->auth_hmac, 0, sizeof(beacon->auth_hmac));
    bramble_beacon_serialize(beacon, buf, sizeof(buf));
    /* HMAC over everything before auth_hmac: BEACON_SIZE - sizeof(auth_hmac) = 32 bytes */
    size_t hmac_len = BEACON_SIZE - sizeof(beacon->auth_hmac);
    uint8_t full_hmac[32];
    crypto_hmac_sha256(shared_key, key_len, buf, hmac_len, full_hmac);
    memcpy(beacon->auth_hmac, full_hmac, sizeof(beacon->auth_hmac));
}

bool beacon_verify_hmac(const bramble_beacon_t* beacon, const uint8_t* shared_key, size_t key_len) {
    bramble_beacon_t copy = *beacon;
    uint8_t saved[sizeof(copy.auth_hmac)];
    memcpy(saved, copy.auth_hmac, sizeof(saved));
    beacon_compute_hmac(&copy, shared_key, key_len);
    /* SEC-H2 (Task 3.4): constant-time compare, OR-accumulate XOR with no
     * early exit, unlike memcmp. The non-constant-time compare was named
     * as part of SEC-H2's root cause. */
    uint8_t r = 0;
    for (size_t i = 0; i < sizeof(saved); i++)
        r |= saved[i] ^ copy.auth_hmac[i];
    return r == 0;
}
