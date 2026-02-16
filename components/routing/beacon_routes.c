#include "beacon_routes.h"
#include <string.h>

void beacon_select_route_ads(const routing_table_t *table, beacon_route_ads_t *ads) {
    memset(ads, 0, sizeof(*ads));

    // Collect eligible routes (ACTIVE, STALE, or UNVERIFIED — not BROKEN or DISCOVERING)
    typedef struct { int idx; uint8_t metric; } candidate_t;
    candidate_t candidates[MAX_ROUTES];
    int num_candidates = 0;

    for (int i = 0; i < table->count; i++) {
        const route_entry_t *r = &table->entries[i];
        if (r->state == ROUTE_BROKEN || r->state == ROUTE_DISCOVERING) continue;
        candidates[num_candidates].idx = i;
        candidates[num_candidates].metric = r->metric;
        num_candidates++;
    }

    // Sort by metric (lowest = best) — simple selection sort, small N
    for (int i = 0; i < num_candidates - 1; i++) {
        int best = i;
        for (int j = i + 1; j < num_candidates; j++) {
            if (candidates[j].metric < candidates[best].metric) {
                best = j;
            }
        }
        if (best != i) {
            candidate_t tmp = candidates[i];
            candidates[i] = candidates[best];
            candidates[best] = tmp;
        }
    }

    // Take top BEACON_MAX_ROUTE_ADS
    int count = num_candidates < BEACON_MAX_ROUTE_ADS ? num_candidates : BEACON_MAX_ROUTE_ADS;
    ads->count = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        const route_entry_t *r = &table->entries[candidates[i].idx];
        ads->routes[i].dest_addr = r->dest_addr;
        ads->routes[i].metric = r->metric;
        ads->routes[i].hop_count = r->hop_count;
        ads->routes[i].flags = 0;
    }
}

void beacon_process_route_ads(routing_table_t *table, uint32_t beacon_src,
                               const beacon_route_ads_t *ads, uint32_t now_ms) {
    for (int i = 0; i < ads->count; i++) {
        const beacon_route_ad_t *ad = &ads->routes[i];
        route_entry_t *existing = route_lookup(table, ad->dest_addr);

        uint8_t new_metric = ad->metric + 1;  // Add cost of one more hop
        uint8_t new_hops = ad->hop_count + 1;

        if (existing) {
            // Only overwrite if new route is strictly better
            if (new_metric < existing->metric) {
                existing->next_hop = beacon_src;
                existing->hop_count = new_hops;
                existing->metric = new_metric;
                existing->state = ROUTE_UNVERIFIED;
                existing->last_confirmed = now_ms;
            }
        } else {
            route_install(table, ad->dest_addr, beacon_src, new_hops,
                         new_metric, ROUTE_UNVERIFIED, now_ms);
        }
    }
}

int beacon_route_ads_serialize(const beacon_route_ads_t *ads, uint8_t *buf, size_t len) {
    size_t needed = 1 + (size_t)ads->count * BEACON_ROUTE_AD_SIZE;
    if (len < needed) return -1;

    buf[0] = ads->count;
    for (int i = 0; i < ads->count; i++) {
        uint8_t *p = buf + 1 + i * BEACON_ROUTE_AD_SIZE;
        memcpy(p, &ads->routes[i].dest_addr, 4);
        p[4] = ads->routes[i].metric;
        p[5] = ads->routes[i].hop_count;
        p[6] = ads->routes[i].flags;
        p[7] = 0; // reserved
    }

    return (int)needed;
}

int beacon_route_ads_deserialize(beacon_route_ads_t *ads, const uint8_t *buf, size_t len) {
    if (len < 1) return -1;

    uint8_t count = buf[0];
    if (count > BEACON_MAX_ROUTE_ADS) return -1;

    size_t needed = 1 + (size_t)count * BEACON_ROUTE_AD_SIZE;
    if (len < needed) return -1;

    ads->count = count;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = buf + 1 + i * BEACON_ROUTE_AD_SIZE;
        memcpy(&ads->routes[i].dest_addr, p, 4);
        ads->routes[i].metric = p[4];
        ads->routes[i].hop_count = p[5];
        ads->routes[i].flags = p[6];
    }

    return (int)needed;
}
