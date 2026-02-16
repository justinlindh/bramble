#ifndef BRAMBLE_BEACON_H
#define BRAMBLE_BEACON_H

#include "packet.h"
#include <stdbool.h>

bramble_beacon_t beacon_build(uint32_t my_addr, uint32_t pubkey_hash,
    uint16_t uptime_min, uint8_t battery_pct, uint8_t tx_queue_depth,
    uint8_t neighbor_count, uint8_t flags, uint32_t network_time,
    uint16_t time_confidence);

void beacon_compute_hmac(bramble_beacon_t *beacon, const uint8_t *shared_key, size_t key_len);
bool beacon_verify_hmac(const bramble_beacon_t *beacon, const uint8_t *shared_key, size_t key_len);

#endif
