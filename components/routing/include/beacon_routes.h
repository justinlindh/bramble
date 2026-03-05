#ifndef BRAMBLE_BEACON_ROUTES_H
#define BRAMBLE_BEACON_ROUTES_H

#include <stdint.h>
#include <stdbool.h>
#include "routing.h"

// Beacons can optionally carry route advertisements
// Format: beacon base (36 bytes) + route_count(1) + routes(N * 8 bytes)
// Each route: dest_addr(4) + metric(1) + hop_count(1) + flags(1) + reserved(1)

#define BEACON_MAX_ROUTE_ADS 4
#define BEACON_ROUTE_AD_SIZE 8

typedef struct {
    uint32_t dest_addr;
    uint8_t metric;
    uint8_t hop_count;
    uint8_t flags; // bit 0: bidirectional confirmed
} beacon_route_ad_t;

typedef struct {
    uint8_t count;
    beacon_route_ad_t routes[BEACON_MAX_ROUTE_ADS];
} beacon_route_ads_t;

// Select best routes to advertise from routing table
// Picks top N by metric (lowest first), excludes BROKEN and DISCOVERING routes
void beacon_select_route_ads(const routing_table_t* table, beacon_route_ads_t* ads);

// Process received route advertisements
// Installs/updates routes with state UNVERIFIED and hop_count+1
void beacon_process_route_ads(routing_table_t* table, uint32_t beacon_src,
                              const beacon_route_ads_t* ads, uint32_t now_ms);

// Serialize route ads to buffer (appended after base beacon)
// Returns number of bytes written, or -1 on error
int beacon_route_ads_serialize(const beacon_route_ads_t* ads, uint8_t* buf, size_t len);

// Deserialize route ads from buffer
// Returns number of bytes consumed, or -1 on error
int beacon_route_ads_deserialize(beacon_route_ads_t* ads, const uint8_t* buf, size_t len);

#endif
