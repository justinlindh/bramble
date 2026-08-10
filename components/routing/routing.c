#include "include/routing.h"
#include <string.h>

/* ── Neighbor table ── */

void neighbor_init(neighbor_table_t* table) { memset(table, 0, sizeof(*table)); }

int neighbor_update(neighbor_table_t* table, uint32_t addr, int8_t rssi, int8_t snr,
                    uint32_t pubkey_hash, uint32_t now_ms) {
    /* Update existing */
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].addr == addr) {
            table->entries[i].rssi = rssi;
            table->entries[i].snr = snr;
            table->entries[i].pubkey_hash = pubkey_hash;
            table->entries[i].last_heard = now_ms;
            if (table->entries[i].beacon_count < 0xFFFF)
                table->entries[i].beacon_count++;
            return i;
        }
    }
    /* Add new */
    int idx;
    if (table->count < MAX_NEIGHBORS) {
        idx = table->count++;
    } else {
        /* Evict oldest */
        idx = 0;
        for (int i = 1; i < MAX_NEIGHBORS; i++) {
            if (table->entries[i].last_heard < table->entries[idx].last_heard)
                idx = i;
        }
    }
    memset(&table->entries[idx], 0, sizeof(neighbor_entry_t));
    table->entries[idx].addr = addr;
    table->entries[idx].rssi = rssi;
    table->entries[idx].snr = snr;
    table->entries[idx].pubkey_hash = pubkey_hash;
    table->entries[idx].last_heard = now_ms;
    table->entries[idx].first_seen_ms = now_ms;
    table->entries[idx].beacon_count = 1;
    /* Defaults until reliability/airtime telemetry is populated from runtime stats */
    table->entries[idx].delivery_rate = 255;     /* 100% */
    table->entries[idx].airtime_remaining = 100; /* 100% */
    return idx;
}

bool neighbor_touch(neighbor_table_t* table, uint32_t addr, int8_t rssi, int8_t snr,
                    uint32_t now_ms) {
    neighbor_entry_t* e = neighbor_lookup(table, addr);
    /* Monotonic only: a frame dispatched out of order relative to the sample
     * that stamped last_heard must not un-age the peer. */
    if (!e || now_ms <= e->last_heard)
        return false;
    e->last_heard = now_ms;
    e->rssi = rssi;
    e->snr = snr;
    /* beacon_count and first_seen_ms are deliberately untouched: tenure
     * (neighbor_is_established) is the anti-Sybil lever and stays gated on
     * sustained beaconing, not on traffic a peer chooses to send us. */
    return true;
}

neighbor_entry_t* neighbor_lookup(neighbor_table_t* table, uint32_t addr) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].addr == addr)
            return &table->entries[i];
    }
    return NULL;
}

void neighbor_purge(neighbor_table_t* table, uint32_t now_ms) {
    int i = 0;
    while (i < table->count) {
        if (now_ms - table->entries[i].last_heard >= NEIGHBOR_EXPIRY_MS) {
            table->entries[i] = table->entries[table->count - 1];
            table->count--;
        } else {
            i++;
        }
    }
}

int neighbor_count(const neighbor_table_t* table) { return table->count; }

bool neighbor_is_established(const neighbor_table_t* table, uint32_t addr, uint32_t now_ms) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].addr == addr) {
            const neighbor_entry_t* e = &table->entries[i];
            return e->beacon_count >= ESTABLISHED_MIN_BEACONS &&
                   (now_ms - e->first_seen_ms) >= ESTABLISHED_MIN_AGE_MS;
        }
    }
    return false;
}

bool neighbor_is_newly_admitted(const neighbor_table_t* table, int idx, uint32_t now_ms) {
    if (!table || idx < 0 || idx >= table->count)
        return false;
    const neighbor_entry_t* e = &table->entries[idx];
    return e->beacon_count == 1 && e->first_seen_ms == now_ms;
}

/* ── Routing table ── */

void route_init(routing_table_t* table) { memset(table, 0, sizeof(*table)); }

static void route_remove(routing_table_t* table, int idx) {
    table->entries[idx] = table->entries[table->count - 1];
    table->count--;
}

int route_install(routing_table_t* table, uint32_t dest, uint32_t next_hop, uint8_t hop_count,
                  uint8_t metric, route_state_t state, route_source_t source, uint32_t now_ms) {
    /* Check existing */
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest) {
            route_entry_t* e = &table->entries[i];
            bool replace;
            /* Task 4-fix F2: trust class dominates. A DISCOVERED
             * (control-plane, HMAC-gated) install always reclaims a
             * BREADCRUMB entry, and a BREADCRUMB install can never displace
             * a DISCOVERED one -- otherwise a forged/maxed breadcrumb
             * (metric 255, 1 hop) would permanently lock out the real
             * route. Same-class installs keep the usual metric/hop rule. */
            if (source != e->source) {
                replace = (source == ROUTE_SRC_DISCOVERED);
            } else {
                /* Replace if: current is broken/stale, or new metric is
                 * better, or same metric fewer hops */
                replace = (e->state == ROUTE_BROKEN || e->state == ROUTE_STALE ||
                           metric > e->metric || (metric == e->metric && hop_count < e->hop_count));
            }
            if (replace) {
                e->next_hop = next_hop;
                e->hop_count = hop_count;
                e->metric = metric;
                e->state = state;
                e->source = source;
                e->fail_count = 0;
                e->last_used = now_ms;
                e->last_confirmed = now_ms;
            }
            return i;
        }
    }
    /* Need a slot */
    int idx;
    if (table->count < MAX_ROUTES) {
        idx = table->count++;
    } else {
        /* Evict: same broken -> stale -> LRU ordering as before, but biased
         * to prefer a ROUTE_SRC_BREADCRUMB victim over a ROUTE_SRC_DISCOVERED
         * one (finding 2, final whole-branch review). A breadcrumb carries
         * only an unauthenticated link hint (see route_source_t doc comment
         * above); a full table should give up a breadcrumb before it gives
         * up a control-plane-verified DISCOVERED route, even when the
         * install needing the slot is itself DISCOVERED. Search breadcrumbs
         * first, in the usual broken -> stale -> LRU order among just that
         * class. */
        idx = -1;
        for (int i = 0; i < table->count; i++) {
            if (table->entries[i].source == ROUTE_SRC_BREADCRUMB &&
                table->entries[i].state == ROUTE_BROKEN) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            for (int i = 0; i < table->count; i++) {
                if (table->entries[i].source == ROUTE_SRC_BREADCRUMB &&
                    table->entries[i].state == ROUTE_STALE) {
                    idx = i;
                    break;
                }
            }
        }
        if (idx < 0) {
            for (int i = 0; i < table->count; i++) {
                if (table->entries[i].source == ROUTE_SRC_BREADCRUMB &&
                    (idx < 0 || table->entries[i].last_used < table->entries[idx].last_used)) {
                    idx = i;
                }
            }
        }
        if (idx < 0) {
            /* No breadcrumb victim exists anywhere in the table. A
             * BREADCRUMB install must not evict a DISCOVERED route to make
             * room for itself either -- that is the exact same trust-class
             * inversion the same-dest F2 rule above forbids, just reached
             * via capacity pressure instead of a same-dest collision -- so
             * refuse the install rather than reclaim a higher-trust entry.
             * A DISCOVERED install has no such restriction and falls
             * through to the original broken -> stale -> LRU search across
             * every entry regardless of source. */
            if (source == ROUTE_SRC_BREADCRUMB) {
                return -1;
            }
            for (int i = 0; i < table->count; i++) {
                if (table->entries[i].state == ROUTE_BROKEN) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                for (int i = 0; i < table->count; i++) {
                    if (table->entries[i].state == ROUTE_STALE) {
                        idx = i;
                        break;
                    }
                }
            }
            if (idx < 0) {
                idx = 0;
                for (int i = 1; i < table->count; i++) {
                    if (table->entries[i].last_used < table->entries[idx].last_used)
                        idx = i;
                }
            }
        }
    }
    memset(&table->entries[idx], 0, sizeof(route_entry_t));
    table->entries[idx].dest_addr = dest;
    table->entries[idx].next_hop = next_hop;
    table->entries[idx].hop_count = hop_count;
    table->entries[idx].metric = metric;
    table->entries[idx].state = state;
    table->entries[idx].source = source;
    table->entries[idx].last_used = now_ms;
    table->entries[idx].last_confirmed = now_ms;
    return idx;
}

route_entry_t* route_lookup(routing_table_t* table, uint32_t dest_addr) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest_addr)
            return &table->entries[i];
    }
    return NULL;
}

void route_maintenance(routing_table_t* table, uint32_t now_ms) {
    int i = 0;
    while (i < table->count) {
        route_entry_t* e = &table->entries[i];
        uint32_t age = now_ms - e->last_confirmed;
        if (age >= ROUTE_HARD_TIMEOUT_MS) {
            route_remove(table, i);
            continue;
        }
        if (e->state == ROUTE_STALE && age >= ROUTE_STALE_TIMEOUT_MS) {
            route_remove(table, i);
            continue;
        }
        if (e->state == ROUTE_ACTIVE && age >= ROUTE_ACTIVE_TIMEOUT_MS) {
            e->state = ROUTE_STALE;
        }
        i++;
    }
}

int route_count(const routing_table_t* table) { return table->count; }

/* ── RREQ dedup ── */

void rreq_dedup_init(rreq_dedup_t* cache) { memset(cache, 0, sizeof(*cache)); }

static void rreq_dedup_purge(rreq_dedup_t* cache, uint32_t now_ms) {
    int i = 0;
    while (i < cache->count) {
        if (now_ms - cache->entries[i].timestamp >= RREQ_DEDUP_EXPIRY_MS) {
            cache->entries[i] = cache->entries[cache->count - 1];
            cache->count--;
        } else {
            i++;
        }
    }
}

bool rreq_dedup_check_and_add(rreq_dedup_t* cache, uint32_t query_id, uint32_t now_ms) {
    rreq_dedup_purge(cache, now_ms);
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].query_id == query_id)
            return true;
    }
    if (cache->count < RREQ_DEDUP_MAX) {
        cache->entries[cache->count].query_id = query_id;
        cache->entries[cache->count].timestamp = now_ms;
        cache->count++;
    }
    return false;
}

/* ── Reverse routes ── */

void reverse_route_init(reverse_route_table_t* table) { memset(table, 0, sizeof(*table)); }

void reverse_route_purge(reverse_route_table_t* table, uint32_t now_ms) {
    int i = 0;
    while (i < table->count) {
        if (now_ms - table->entries[i].timestamp >= REVERSE_ROUTE_EXPIRY_MS) {
            table->entries[i] = table->entries[table->count - 1];
            table->count--;
        } else {
            i++;
        }
    }
}

int reverse_route_add(reverse_route_table_t* table, uint32_t query_id, uint32_t prev_hop,
                      uint32_t now_ms) {
    reverse_route_purge(table, now_ms);
    if (table->count >= MAX_REVERSE_ROUTES) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < table->count; i++) {
            if (table->entries[i].timestamp < table->entries[oldest].timestamp)
                oldest = i;
        }
        table->entries[oldest] = table->entries[table->count - 1];
        table->count--;
    }
    int idx = table->count++;
    table->entries[idx].query_id = query_id;
    table->entries[idx].prev_hop = prev_hop;
    table->entries[idx].timestamp = now_ms;
    return idx;
}

reverse_route_t* reverse_route_lookup(reverse_route_table_t* table, uint32_t query_id) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].query_id == query_id)
            return &table->entries[i];
    }
    return NULL;
}

/* ── Link penalty ── */

uint8_t compute_link_penalty(int8_t rssi, int8_t snr) {
    /* Linear RSSI penalty from strong to weak receive conditions.
     *
     * Note: SX1262 sensitivity floor varies by SF/BW combination, so
     * PENALTY_RSSI_WORST is an approximate routing heuristic rather than
     * a single PHY decode threshold.
     */
    int rp = 0;
    if (rssi <= PENALTY_RSSI_WORST)
        rp = PENALTY_RSSI_WEIGHT;
    else if (rssi >= PENALTY_RSSI_BEST)
        rp = 0;
    else
        rp = (int)(PENALTY_RSSI_BEST - rssi) * PENALTY_RSSI_WEIGHT /
             (PENALTY_RSSI_BEST - PENALTY_RSSI_WORST);

    /* Linear SNR penalty from healthy margin down to low/negative margin.
     * SX1262 demod margin also varies with SF/BW, so these bounds are tuned
     * for route scoring consistency, not absolute radio limits.
     */
    int sp = 0;
    if (snr <= PENALTY_SNR_WORST)
        sp = PENALTY_SNR_WEIGHT;
    else if (snr >= PENALTY_SNR_BEST)
        sp = 0;
    else
        sp = (PENALTY_SNR_BEST - (int)snr) * PENALTY_SNR_WEIGHT /
             (PENALTY_SNR_BEST - PENALTY_SNR_WORST);

    int total = rp + sp;
    return (uint8_t)(total > PENALTY_MAX_TOTAL ? PENALTY_MAX_TOTAL : total);
}

uint8_t metric_apply_link_penalty(uint8_t metric, int8_t rssi, int8_t snr) {
    uint8_t penalty = compute_link_penalty(rssi, snr);
    return (penalty >= metric) ? 0 : (uint8_t)(metric - penalty);
}
