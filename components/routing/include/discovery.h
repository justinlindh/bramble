#ifndef BRAMBLE_DISCOVERY_H
#define BRAMBLE_DISCOVERY_H

#include "packet.h"
#include "routing.h"
#include <stdbool.h>

#define MAX_PENDING_DISCOVERIES 8
#define RREQ_RETRY_INTERVAL_1_MS 5000
#define RREQ_RETRY_INTERVAL_2_MS 15000
#define MAX_RREQ_ATTEMPTS 3
#define MAX_QUEUED_PER_DISCOVERY 4

typedef struct {
    uint32_t dest_addr;
    uint32_t query_id;
    uint32_t timestamp;
    uint8_t attempts;
    uint8_t queued_count;
} pending_discovery_t;

typedef struct {
    pending_discovery_t entries[MAX_PENDING_DISCOVERIES];
    int count;
} pending_discovery_table_t;

void discovery_init(pending_discovery_table_t* table);
int discovery_start(pending_discovery_table_t* table, uint32_t dest_addr, uint32_t query_id,
                    uint32_t now_ms);
pending_discovery_t* discovery_lookup(pending_discovery_table_t* table, uint32_t dest_addr);
pending_discovery_t* discovery_lookup_by_query(pending_discovery_table_t* table, uint32_t query_id);
void discovery_remove(pending_discovery_table_t* table, uint32_t dest_addr);
bool discovery_should_retry(const pending_discovery_t* d, uint32_t now_ms);
void discovery_record_attempt(pending_discovery_t* d, uint32_t now_ms);

bramble_rreq_t rreq_build_originator(uint32_t my_addr, uint32_t dest_addr, uint32_t query_id,
                                     uint32_t encrypted_source);
bramble_rreq_t rreq_forward(const bramble_rreq_t* incoming, uint32_t my_addr, int8_t rx_rssi,
                            int8_t rx_snr);
bramble_rrep_t rrep_build_destination(const bramble_rreq_t* rreq, uint32_t my_addr);
bramble_rrep_t rrep_forward(const bramble_rrep_t* incoming, uint32_t next_hop_back);

#endif
