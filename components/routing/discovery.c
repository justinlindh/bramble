#include "include/discovery.h"
#include <string.h>

void discovery_init(pending_discovery_table_t* table) { memset(table, 0, sizeof(*table)); }

int discovery_start(pending_discovery_table_t* table, uint32_t dest_addr, uint32_t query_id,
                    uint32_t now_ms) {
    if (table->count >= MAX_PENDING_DISCOVERIES)
        return -1;
    pending_discovery_t* e = &table->entries[table->count++];
    e->dest_addr = dest_addr;
    e->query_id = query_id;
    e->timestamp = now_ms;
    e->attempts = 1;
    e->queued_count = 0;
    return 0;
}

pending_discovery_t* discovery_lookup(pending_discovery_table_t* table, uint32_t dest_addr) {
    for (int i = 0; i < table->count; i++)
        if (table->entries[i].dest_addr == dest_addr)
            return &table->entries[i];
    return NULL;
}

pending_discovery_t* discovery_lookup_by_query(pending_discovery_table_t* table,
                                               uint32_t query_id) {
    for (int i = 0; i < table->count; i++)
        if (table->entries[i].query_id == query_id)
            return &table->entries[i];
    return NULL;
}

void discovery_remove(pending_discovery_table_t* table, uint32_t dest_addr) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest_addr) {
            table->entries[i] = table->entries[--table->count];
            return;
        }
    }
}

bool discovery_should_retry(const pending_discovery_t* d, uint32_t now_ms) {
    if (d->attempts >= MAX_RREQ_ATTEMPTS)
        return false;
    uint32_t interval = (d->attempts <= 1) ? RREQ_RETRY_INTERVAL_1_MS : RREQ_RETRY_INTERVAL_2_MS;
    return (now_ms - d->timestamp) >= interval;
}

void discovery_record_attempt(pending_discovery_t* d, uint32_t now_ms) {
    d->attempts++;
    d->timestamp = now_ms;
}

bramble_rreq_t rreq_build_originator(uint32_t my_addr, uint32_t dest_addr, uint32_t query_id,
                                     uint32_t encrypted_source) {
    bramble_rreq_t r;
    memset(&r, 0, sizeof(r));
    r.header.version = BRAMBLE_VERSION;
    r.header.type = PKT_TYPE_RREQ;
    r.header.flags = 0;
    r.header.hop_limit = 4;
    r.header.dest_addr = dest_addr;
    r.header.packet_id = query_id;
    r.query_id = query_id;
    r.encrypted_source = encrypted_source;
    r.hop_count = 0;
    r.metric = 255;
    r.prev_hop = my_addr;
    return r;
}

bramble_rreq_t rreq_forward(const bramble_rreq_t* incoming, uint32_t my_addr, int8_t rx_rssi,
                            int8_t rx_snr) {
    bramble_rreq_t r = *incoming;
    r.hop_count++;
    uint8_t penalty = compute_link_penalty(rx_rssi, rx_snr);
    r.metric = (penalty >= r.metric) ? 0 : r.metric - penalty;
    r.header.hop_limit--;
    r.prev_hop = my_addr;
    return r;
}

bramble_rrep_t rrep_build_destination(const bramble_rreq_t* rreq, uint32_t my_addr) {
    bramble_rrep_t r;
    memset(&r, 0, sizeof(r));
    r.header.version = BRAMBLE_VERSION;
    r.header.type = PKT_TYPE_RREP;
    r.header.flags = 0;
    r.header.hop_limit = 4;
    r.header.dest_addr = rreq->prev_hop; /* unicast back toward originator */
    r.header.packet_id = rreq->query_id;
    r.query_id = rreq->query_id;
    r.src_addr = my_addr;
    r.next_hop = rreq->prev_hop;
    r.hop_count = rreq->hop_count + 1;
    r.route_metric = rreq->metric;
    return r;
}

bramble_rrep_t rrep_forward(const bramble_rrep_t* incoming, uint32_t next_hop_back) {
    bramble_rrep_t r = *incoming;
    r.next_hop = next_hop_back;
    r.header.dest_addr = next_hop_back;
    return r;
}
