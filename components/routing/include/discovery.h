#ifndef BRAMBLE_DISCOVERY_H
#define BRAMBLE_DISCOVERY_H

#include "packet.h"
#include "routing.h"
#include <stdbool.h>

#define MAX_PENDING_DISCOVERIES 8
#define RREQ_RETRY_INTERVAL_1_MS 5000
#define RREQ_RETRY_INTERVAL_2_MS 15000
#define MAX_RREQ_ATTEMPTS 3

/* Expanding-ring discovery: the first attempt floods with a conservative hop
 * budget; retries widen to the protocol's maximum route depth. Each retry
 * carries a fresh query_id (and therefore a fresh originator pseudonym), so
 * retries are not swallowed by the RREQ dedup window on nodes that heard an
 * earlier attempt. */
#define RREQ_HOP_LIMIT_INITIAL 4
#define RREQ_HOP_LIMIT_EXPANDED ROUTE_HOP_LIMIT_MAX

/* Relays delay RREQ rebroadcast by a random jitter in this range so same-hop
 * relays do not key up at the same instant (same window channel_flood uses). */
#define RREQ_FWD_JITTER_MIN_MS 50
#define RREQ_FWD_JITTER_MAX_MS 300

typedef struct {
    uint32_t dest_addr;
    uint32_t query_ids[MAX_RREQ_ATTEMPTS]; /* one fresh query_id per attempt */
    uint32_t timestamp;
    uint8_t attempts;
} pending_discovery_t;

typedef struct {
    pending_discovery_t entries[MAX_PENDING_DISCOVERIES];
    int count;
} pending_discovery_table_t;

void discovery_init(pending_discovery_table_t* table);
int discovery_start(pending_discovery_table_t* table, uint32_t dest_addr, uint32_t query_id,
                    uint32_t now_ms);
pending_discovery_t* discovery_lookup(pending_discovery_table_t* table, uint32_t dest_addr);
/* Matches an RREP against ANY query_id of any outstanding attempt, so a late
 * answer to an earlier attempt still completes the discovery. */
pending_discovery_t* discovery_lookup_by_query(pending_discovery_table_t* table, uint32_t query_id);
void discovery_remove(pending_discovery_table_t* table, uint32_t dest_addr);
bool discovery_should_retry(const pending_discovery_t* d, uint32_t now_ms);
/* Records a retry attempt under a freshly generated query_id. */
void discovery_record_attempt(pending_discovery_t* d, uint32_t query_id, uint32_t now_ms);
uint32_t discovery_current_query_id(const pending_discovery_t* d);
/* Expanding ring: attempt 1 uses RREQ_HOP_LIMIT_INITIAL, retries use
 * RREQ_HOP_LIMIT_EXPANDED. */
uint8_t discovery_hop_limit_for_attempt(uint8_t attempt);
/* Maps a random value into [RREQ_FWD_JITTER_MIN_MS, RREQ_FWD_JITTER_MAX_MS]. */
uint32_t discovery_forward_jitter_ms(uint32_t random_value);

bramble_rreq_t rreq_build_originator(uint32_t my_addr, uint32_t dest_addr, uint32_t query_id,
                                     uint32_t encrypted_source, uint8_t hop_limit);
bramble_rreq_t rreq_forward(const bramble_rreq_t* incoming, uint32_t my_addr, int8_t rx_rssi,
                            int8_t rx_snr);
bramble_rrep_t rrep_build_destination(const bramble_rreq_t* rreq, uint32_t my_addr);
bramble_rrep_t rrep_forward(const bramble_rrep_t* incoming, uint32_t next_hop_back);

#endif
