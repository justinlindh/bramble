#ifndef BRAMBLE_SECURITY_H
#define BRAMBLE_SECURITY_H
#include <stdint.h>
#include <stdbool.h>

#define RREQ_RATE_LIMIT_MS 30000
#define RREQ_RATE_ENTRIES 64
#define SYBIL_RSSI_CLUSTER_THRESHOLD 3
#define SYBIL_MIN_SUSPECTS 3

typedef struct {
    uint32_t neighbor_addr;
    uint32_t dest_addr;
    uint32_t last_rreq_ms;
} rreq_rate_entry_t;

typedef struct {
    rreq_rate_entry_t entries[RREQ_RATE_ENTRIES];
    int count;
} rreq_rate_limiter_t;

void rreq_rate_init(rreq_rate_limiter_t *rl);
bool rreq_rate_allow(rreq_rate_limiter_t *rl, uint32_t neighbor, uint32_t dest, uint32_t now_ms);
bool sybil_check_rssi_cluster(const int8_t *rssi_values, int count);
#endif
