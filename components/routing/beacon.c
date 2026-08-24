#include "beacon.h"
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
    /* Serialize beacon with zeroed HMAC, then compute over everything
     * EXCEPT auth_hmac itself. Buffer must be large enough for name
     * extension (BEACON_SIZE + 1 + BEACON_NAME_MAX). */
    uint8_t buf[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    memset(beacon->auth_hmac, 0, sizeof(beacon->auth_hmac));
    bramble_beacon_serialize(beacon, buf, sizeof(buf));

    /* Cover the optional name field too, not just the fixed prefix before
     * auth_hmac. That prefix length is always BEACON_SIZE -
     * sizeof(auth_hmac), derived below and never hardcoded, so growing the
     * fixed fields cannot silently drop bytes out of coverage.
     * bramble_beacon_serialize lays the wire out as
     * [fixed prefix | auth_hmac (16) | name_len + name (0 or 1+nlen)], so
     * the HMAC input is the fixed prefix concatenated with the name
     * region, skipping over auth_hmac's own 16 bytes in the middle (which
     * must stay excluded from their own coverage). Without this, an
     * attacker rewrites any captured beacon's name and it still verifies,
     * leaving the display name spoofable even under a provisioned key. */
    size_t prefix_len = BEACON_SIZE - sizeof(beacon->auth_hmac);
    size_t name_wire_len = bramble_beacon_wire_size(beacon) - BEACON_SIZE; /* 0, or 1+nlen */
    uint8_t hmac_input[(BEACON_SIZE - 16) + 1 + BEACON_NAME_MAX];
    memcpy(hmac_input, buf, prefix_len);
    if (name_wire_len > 0) {
        memcpy(hmac_input + prefix_len, buf + BEACON_SIZE, name_wire_len);
    }
    uint8_t full_hmac[32];
    crypto_hmac_sha256(shared_key, key_len, hmac_input, prefix_len + name_wire_len, full_hmac);
    memcpy(beacon->auth_hmac, full_hmac, sizeof(beacon->auth_hmac));
}

bool beacon_verify_hmac(const bramble_beacon_t* beacon, const uint8_t* shared_key, size_t key_len) {
    bramble_beacon_t copy = *beacon;
    uint8_t saved[sizeof(copy.auth_hmac)];
    memcpy(saved, copy.auth_hmac, sizeof(saved));
    beacon_compute_hmac(&copy, shared_key, key_len);
    /* SEC-H2: constant-time compare with no early exit, unlike memcmp. Uses
     * the shared crypto_ct_memeq so every tag/MAC check in the tree goes
     * through one implementation. */
    return crypto_ct_memeq(saved, copy.auth_hmac, sizeof(saved));
}
