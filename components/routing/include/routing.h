#ifndef BRAMBLE_ROUTING_H
#define BRAMBLE_ROUTING_H
#include <stdint.h>
#include <stdbool.h>

#define MAX_NEIGHBORS 32
#define NEIGHBOR_EXPIRY_MS 600000

typedef struct {
    uint32_t addr;
    int8_t   rssi;
    int8_t   snr;
    uint8_t  success_rate;
    uint8_t  congestion;
    uint32_t last_heard;
    uint32_t pubkey_hash;
    uint16_t tx_count;
    uint16_t tx_success;
    bool     suspicious;
} neighbor_entry_t;

typedef struct {
    neighbor_entry_t entries[MAX_NEIGHBORS];
    int count;
} neighbor_table_t;

void neighbor_init(neighbor_table_t *table);
int  neighbor_update(neighbor_table_t *table, uint32_t addr, int8_t rssi, int8_t snr, uint32_t pubkey_hash, uint32_t now_ms);
neighbor_entry_t *neighbor_lookup(neighbor_table_t *table, uint32_t addr);
void neighbor_purge(neighbor_table_t *table, uint32_t now_ms);
int  neighbor_count(const neighbor_table_t *table);

#define MAX_ROUTES 64
#define ROUTE_ACTIVE_TIMEOUT_MS   300000
#define ROUTE_STALE_TIMEOUT_MS    600000
#define ROUTE_HARD_TIMEOUT_MS     3600000

typedef enum {
    ROUTE_DISCOVERING = 0, ROUTE_UNVERIFIED, ROUTE_ACTIVE, ROUTE_STALE, ROUTE_BROKEN,
} route_state_t;

typedef struct {
    uint32_t     dest_addr;
    uint32_t     next_hop;
    uint8_t      hop_count;
    uint8_t      metric;
    route_state_t state;
    uint8_t      fail_count;
    uint32_t     last_used;
    uint32_t     last_confirmed;
    uint16_t     use_count;
} route_entry_t;

typedef struct {
    route_entry_t entries[MAX_ROUTES];
    int count;
} routing_table_t;

void  route_init(routing_table_t *table);
int   route_install(routing_table_t *table, uint32_t dest, uint32_t next_hop, uint8_t hop_count, uint8_t metric, route_state_t state, uint32_t now_ms);
route_entry_t *route_lookup(routing_table_t *table, uint32_t dest_addr);
void  route_maintenance(routing_table_t *table, uint32_t now_ms);
int   route_count(const routing_table_t *table);
route_entry_t *route_find_alternate(routing_table_t *table, uint32_t dest, uint32_t exclude_hop);

#define RREQ_DEDUP_MAX 128
#define RREQ_DEDUP_EXPIRY_MS 30000

typedef struct { uint32_t query_id; uint32_t timestamp; } rreq_seen_t;
typedef struct { rreq_seen_t entries[RREQ_DEDUP_MAX]; int count; } rreq_dedup_t;

void rreq_dedup_init(rreq_dedup_t *cache);
bool rreq_dedup_check_and_add(rreq_dedup_t *cache, uint32_t query_id, uint32_t now_ms);

#define MAX_REVERSE_ROUTES 32
#define REVERSE_ROUTE_EXPIRY_MS 60000

typedef struct { uint32_t query_id; uint32_t prev_hop; uint32_t timestamp; } reverse_route_t;
typedef struct { reverse_route_t entries[MAX_REVERSE_ROUTES]; int count; } reverse_route_table_t;

void reverse_route_init(reverse_route_table_t *table);
int  reverse_route_add(reverse_route_table_t *table, uint32_t query_id, uint32_t prev_hop, uint32_t now_ms);
reverse_route_t *reverse_route_lookup(reverse_route_table_t *table, uint32_t query_id);
void reverse_route_purge(reverse_route_table_t *table, uint32_t now_ms);

uint8_t compute_link_penalty(int8_t rssi, int8_t snr);
#endif
