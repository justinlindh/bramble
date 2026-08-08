#ifndef BRAMBLE_NODE_PRESENCE_H
#define BRAMBLE_NODE_PRESENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure presentation helpers for peer recency, shared by the Nodes list and the
 * node detail card. Kept out of the LVGL screen files so the arithmetic and the
 * formatting are host-testable (same pattern as chat_message_ui.c). */

/* A peer heard longer ago than this reads as "stale": its row is dimmed so live
 * nodes stand out. Distinct from NEIGHBOR_EXPIRY_MS (10 min), the point at
 * which routing actually evicts the entry. */
#define NODE_STALE_AGE_S 300

/* A peer heard within this reads as "online" to the reachability classifier
 * below. Deliberately much tighter than NODE_STALE_AGE_S, because it answers a
 * different question: NODE_STALE_AGE_S decides when a Nodes row stops looking
 * live, this decides whether a message sent right now has a direct neighbor to
 * land on, and a chat header should say so within a beacon interval or two
 * rather than five minutes. Same value the webapp chat header uses, so the two
 * clients give the same answer about the same peer. */
#define NODE_ONLINE_AGE_S 90

typedef enum {
    NODE_PRESENCE_LIVE = 0,
    NODE_PRESENCE_STALE = 1,
} node_presence_t;

/* Three-way reachability, for surfaces that answer "can I get a message to
 * this peer right now" rather than just "is this row fresh". Mirrors the
 * webapp's chat-header dot so both clients read the same. */
typedef enum {
    NODE_REACH_ONLINE = 0,    /* in the neighbor table and heard recently */
    NODE_REACH_REACHABLE = 1, /* a quiet neighbor, or reachable over an active route */
    NODE_REACH_UNKNOWN = 2,   /* neither: nothing says this peer can be reached */
} node_reach_t;

/* has_neighbor / age_s describe the neighbor-table entry (age_s is ignored
 * when has_neighbor is false); has_active_route is a non-broken, non-stale
 * route to the peer. A neighbor gone quiet past NODE_ONLINE_AGE_S is still
 * REACHABLE rather than UNKNOWN: it has not been purged, so the last thing we
 * know is that it was there. */
node_reach_t node_reach_classify(bool has_neighbor, uint32_t age_s, bool has_active_route);

/* Age of a peer in whole seconds. The two clocks are the same monotonic
 * esp_timer millisecond base, but the caller samples them separately, so a
 * last_heard stamped after the now_ms sample would underflow the unsigned
 * subtraction into a ~49-day age. Clamp that to zero instead. */
uint32_t node_age_seconds(uint32_t now_ms, uint32_t last_heard_ms);

node_presence_t node_presence_for_age(uint32_t age_s);

/* Age that visibly counts up once a second at the ages a live mesh actually
 * shows: "12s", "4m 12s", "2h 14m", "3d 4h". Returns characters written. */
int node_format_age(uint32_t age_s, char* buf, size_t buf_len);

#endif
