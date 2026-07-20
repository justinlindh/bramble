#ifndef BRAMBLE_ROUTING_H
#define BRAMBLE_ROUTING_H
#include <stdint.h>
#include <stdbool.h>

#define MAX_NEIGHBORS 32
#define NEIGHBOR_EXPIRY_MS 600000

typedef struct {
    uint32_t addr;
    int8_t rssi;
    int8_t snr;
    uint8_t success_rate;
    uint32_t last_heard;
    uint32_t pubkey_hash;
    uint16_t tx_count;
    uint16_t tx_success;
    bool suspicious;
    /* Adaptive routing metrics (added for composite route scoring) */
    uint16_t packets_sent;     /* total packets sent to this neighbor */
    uint16_t packets_acked;    /* ACKs received from this neighbor */
    uint8_t delivery_rate;     /* EMA delivery rate 0-255 (255 = 100%) */
    uint8_t airtime_remaining; /* last reported airtime % (from beacon) */
    uint16_t avg_latency_ms;   /* EMA round-trip latency */
    char name[17];             /* node name from beacon (max 16 chars + null) */
    uint32_t first_seen_ms;    /* timestamp of first beacon from this address (tenure start) */
    uint16_t beacon_count;     /* beacons received from this address, saturates at 0xFFFF */
} neighbor_entry_t;

typedef struct {
    neighbor_entry_t entries[MAX_NEIGHBORS];
    int count;
} neighbor_table_t;

void neighbor_init(neighbor_table_t* table);
int neighbor_update(neighbor_table_t* table, uint32_t addr, int8_t rssi, int8_t snr,
                    uint32_t pubkey_hash, uint32_t now_ms);
neighbor_entry_t* neighbor_lookup(neighbor_table_t* table, uint32_t addr);
void neighbor_purge(neighbor_table_t* table, uint32_t now_ms);
int neighbor_count(const neighbor_table_t* table);

/* Tenure thresholds for treating a neighbor as established (ws 1.3c
 * anti-Sybil lever): sustained presence, not just a single beacon. */
#define ESTABLISHED_MIN_BEACONS 3
#define ESTABLISHED_MIN_AGE_MS 300000

/* True iff addr has a table entry with enough beacons over enough elapsed
 * time to be treated as a trusted corroboration source. A re-added neighbor
 * (post-purge) starts fresh, since neighbor_update resets tenure on a new
 * entry. */
bool neighbor_is_established(const neighbor_table_t* table, uint32_t addr, uint32_t now_ms);

#define MAX_ROUTES 64
#define ROUTE_ACTIVE_TIMEOUT_MS 300000
#define ROUTE_STALE_TIMEOUT_MS 600000
#define ROUTE_HARD_TIMEOUT_MS 3600000

/* Maximum route depth the protocol supports end-to-end. Expanded RREQ
 * attempts, RREP returns, DATA forwarding, and RERR teardown all build with
 * this hop budget so a discovered route is usable at full depth
 * (ACK_MAX_HOPS in packet.h matches). */
#define ROUTE_HOP_LIMIT_MAX 8

/* Link-penalty normalization bounds and weights.
 *
 * These model practical LoRa receive quality around SX1262-class sensitivity
 * floors (Semtech SX1261/2 datasheet). Actual minimum decodable RSSI depends
 * on SF/BW (e.g., SF7/125kHz vs SF12/125kHz), so these are conservative,
 * routing-oriented heuristics rather than PHY hard limits.
 */
#define PENALTY_RSSI_BEST (-60)   /* dBm: strong link */
#define PENALTY_RSSI_WORST (-120) /* dBm: near weak-link floor */
#define PENALTY_RSSI_WEIGHT 30

#define PENALTY_SNR_BEST 10    /* dB: strong margin */
#define PENALTY_SNR_WORST (-5) /* dB: low/negative margin */
#define PENALTY_SNR_WEIGHT 20

#define PENALTY_MAX_TOTAL (PENALTY_RSSI_WEIGHT + PENALTY_SNR_WEIGHT)

typedef enum {
    ROUTE_DISCOVERING = 0,
    ROUTE_UNVERIFIED,
    ROUTE_ACTIVE,
    ROUTE_STALE,
    ROUTE_BROKEN,
} route_state_t;

/* Trust class of a route install (Task 4-fix F2). A DATA breadcrumb
 * (dest=src_addr, next_hop=prev_hop, learned off a forwarded DATA frame)
 * carries only immediate-link quality and, critically, an UNAUTHENTICATED
 * next-hop hint (prev_hop is relay-mutable, MAC-excluded). Without a trust
 * class a nearby breadcrumb with a maxed-out (255,1) metric could lock out
 * and never yield to a real DISCOVERED route (RREP/beacon), which is
 * HMAC-authenticated end to end. route_install arbitrates on this first:
 *   - a DISCOVERED install ALWAYS wins over an existing BREADCRUMB entry
 *     (installs/reclaims regardless of metric or hop count);
 *   - a BREADCRUMB install NEVER displaces an existing DISCOVERED entry;
 *   - same-class installs fall through to the usual metric/hop arbitration.
 *
 * The same trust class also governs capacity-eviction victim selection when
 * MAX_ROUTES is full and a new dest needs a slot (route_install): eviction
 * prefers a BREADCRUMB victim (broken -> stale -> LRU, in that order) over a
 * DISCOVERED one; if the table holds no BREADCRUMB entry at all, a
 * BREADCRUMB install is refused rather than evicting a DISCOVERED route,
 * while a DISCOVERED install may still evict any entry.
 */
typedef enum {
    ROUTE_SRC_DISCOVERED = 0, /* RREP/beacon: control-plane, HMAC-authenticated */
    ROUTE_SRC_BREADCRUMB,     /* unauthenticated hint: DATA reverse-route
                               * learning (wire v4) or an RREQ source route
                               * (RREQs carry no HMAC; see issue #74) */
} route_source_t;

typedef struct {
    uint32_t dest_addr;
    uint32_t next_hop;
    uint8_t hop_count;
    uint8_t metric;
    route_state_t state;
    uint8_t fail_count;
    uint32_t last_used;
    uint32_t last_confirmed;
    uint16_t use_count;
    route_source_t source; /* trust class; see route_source_t + route_install */
} route_entry_t;

typedef struct {
    route_entry_t entries[MAX_ROUTES];
    int count;
} routing_table_t;

void route_init(routing_table_t* table);
int route_install(routing_table_t* table, uint32_t dest, uint32_t next_hop, uint8_t hop_count,
                  uint8_t metric, route_state_t state, route_source_t source, uint32_t now_ms);
route_entry_t* route_lookup(routing_table_t* table, uint32_t dest_addr);
void route_maintenance(routing_table_t* table, uint32_t now_ms);
int route_count(const routing_table_t* table);

#define RREQ_DEDUP_MAX 128
#define RREQ_DEDUP_EXPIRY_MS 30000

typedef struct {
    uint32_t query_id;
    uint32_t timestamp;
} rreq_seen_t;
typedef struct {
    rreq_seen_t entries[RREQ_DEDUP_MAX];
    int count;
} rreq_dedup_t;

void rreq_dedup_init(rreq_dedup_t* cache);
/* First-arrival dedup: the first copy of a flood wins; later copies of the
 * same query are dropped regardless of path quality. Path metrics still
 * arbitrate at route_install time among RREPs from different discovery
 * attempts (each attempt floods under a fresh query_id). */
bool rreq_dedup_check_and_add(rreq_dedup_t* cache, uint32_t query_id, uint32_t now_ms);

#define MAX_REVERSE_ROUTES 32
#define REVERSE_ROUTE_EXPIRY_MS 60000

typedef struct {
    uint32_t query_id;
    uint32_t prev_hop;
    uint32_t timestamp;
} reverse_route_t;
typedef struct {
    reverse_route_t entries[MAX_REVERSE_ROUTES];
    int count;
} reverse_route_table_t;

void reverse_route_init(reverse_route_table_t* table);
int reverse_route_add(reverse_route_table_t* table, uint32_t query_id, uint32_t prev_hop,
                      uint32_t now_ms);
reverse_route_t* reverse_route_lookup(reverse_route_table_t* table, uint32_t query_id);
void reverse_route_purge(reverse_route_table_t* table, uint32_t now_ms);

uint8_t compute_link_penalty(int8_t rssi, int8_t snr);
/* Subtracts the link penalty from a higher-is-better path metric, flooring at
 * zero. Used wherever a received metric is extended across the receiving
 * link (RREQ forward, route install at the destination and originator). */
uint8_t metric_apply_link_penalty(uint8_t metric, int8_t rssi, int8_t snr);
#endif
