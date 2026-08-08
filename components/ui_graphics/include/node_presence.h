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

/* Age of a peer in whole seconds. The two clocks are the same monotonic
 * esp_timer millisecond base, but the caller samples them separately, so a
 * last_heard stamped after the now_ms sample would underflow the unsigned
 * subtraction into a ~49-day age. Clamp that to zero instead. */
uint32_t node_age_seconds(uint32_t now_ms, uint32_t last_heard_ms);

/* True once a peer is old enough that its Nodes row should read as faded
 * rather than live. */
bool node_is_stale(uint32_t age_s);

/* Age that visibly counts up once a second at the ages a live mesh actually
 * shows: "12s", "4m 12s", "2h 14m", "3d 4h". Returns characters written. */
int node_format_age(uint32_t age_s, char* buf, size_t buf_len);

/* RSSI as a 0-100 signal-bar percentage, clamping the usable window to
 * -120..-50 dBm. Shared so the Nodes row and the node detail card cannot drift
 * apart on the one number a user compares between the two screens. */
int node_signal_pct(int8_t rssi);

#endif
