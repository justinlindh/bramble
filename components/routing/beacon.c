#include "include/beacon.h"
#include "crypto.h"
#include <string.h>

bramble_beacon_t beacon_build(uint32_t my_addr, uint32_t pubkey_hash,
    uint16_t uptime_min, uint8_t battery_pct, uint8_t tx_queue_depth,
    uint8_t neighbor_count, uint8_t flags, uint32_t network_time,
    uint16_t time_confidence)
{
    bramble_beacon_t b;
    memset(&b, 0, sizeof(b));
    b.header.version = BRAMBLE_VERSION;
    b.header.type = PKT_TYPE_BEACON;
    b.header.flags = 0;
    b.header.hop_limit = 1; /* beacons are single-hop */
    b.header.dest_addr = 0xFFFFFFFF; /* broadcast */
    b.header.packet_id = 0; /* caller should set */
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

void beacon_compute_hmac(bramble_beacon_t *beacon, const uint8_t *shared_key, size_t key_len) {
    /* Serialize beacon with zeroed HMAC, then compute over the pre-HMAC portion */
    uint8_t buf[BEACON_SIZE];
    memset(beacon->auth_hmac, 0, 8);
    bramble_beacon_serialize(beacon, buf, BEACON_SIZE);
    /* HMAC over bytes 0..31 (everything before the auth_hmac field) */
    uint8_t full_hmac[32];
    crypto_hmac_sha256(shared_key, key_len, buf, 32, full_hmac);
    memcpy(beacon->auth_hmac, full_hmac, 8);
}

bool beacon_verify_hmac(const bramble_beacon_t *beacon, const uint8_t *shared_key, size_t key_len) {
    bramble_beacon_t copy = *beacon;
    uint8_t saved[8];
    memcpy(saved, copy.auth_hmac, 8);
    beacon_compute_hmac(&copy, shared_key, key_len);
    return memcmp(saved, copy.auth_hmac, 8) == 0;
}
