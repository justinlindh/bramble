#ifndef BRAMBLE_NODE_PRESENCE_H
#define BRAMBLE_NODE_PRESENCE_H

#include <stddef.h>
#include <stdint.h>

/* Pure presentation helpers for peer recency, shared by the Nodes list and the
 * node detail card. Kept out of the LVGL screen files so the arithmetic and the
 * formatting are host-testable (same pattern as chat_message_ui.c). */

/* A peer heard longer ago than this reads as "stale": its row is dimmed so live
 * nodes stand out. Distinct from NEIGHBOR_EXPIRY_MS (10 min), the point at
 * which routing actually evicts the entry. */
#define NODE_STALE_AGE_S 300

typedef enum {
    NODE_PRESENCE_LIVE = 0,
    NODE_PRESENCE_STALE = 1,
} node_presence_t;

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
