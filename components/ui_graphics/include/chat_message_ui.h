#ifndef CHAT_MESSAGE_UI_H
#define CHAT_MESSAGE_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "msg_store.h"

typedef enum {
    CHAT_DELIVERY_BADGE_PENDING = 0,
    CHAT_DELIVERY_BADGE_SINGLE_CHECK,
    CHAT_DELIVERY_BADGE_DOUBLE_CHECK,
    CHAT_DELIVERY_BADGE_FAILED,
} chat_delivery_badge_kind_t;

typedef enum {
    CHAT_DELIVERY_COLOR_UNDELIVERED = 0,
    CHAT_DELIVERY_COLOR_DELIVERED,
    CHAT_DELIVERY_COLOR_FAILED,
} chat_delivery_color_role_t;

typedef struct {
    chat_delivery_badge_kind_t kind;
    chat_delivery_color_role_t color_role;
} chat_delivery_badge_t;

chat_delivery_badge_t chat_message_delivery_badge(msg_status_t status);
/* An outgoing message with a packet id can always expand its details panel:
 * status, route when it says something, delivery receipts. */
bool chat_message_has_details_toggle(bool is_outgoing, uint32_t packet_id);

/* Whether the expanded panel should draw a route line.
 *
 * A route is only worth showing for a single-recipient message that actually
 * traversed a relay. Two cases must stay silent. A message that reached its
 * recipient directly has a "route" of sender then recipient, which restates
 * the addressing the panel already shows. And a channel or broadcast message
 * has one route per recipient, while the store keeps a single route field
 * that each arriving receipt overwrites, so any line drawn from it presents
 * one arbitrary recipient's path as the whole message's path.
 *
 * hop_count counts endpoints, so a relayed path is 3 or more. */
bool chat_message_route_is_informative(int16_t channel_index, uint8_t route_hop_count);

/* Formats a receipt summary like "Delivered to 3: Alic, Bob, Carl" or
 * "Delivered to 5: Alic, Bob, Carl, Dave, +1". addrs holds the first
 * shown_count unique recipients; total is the full unique-recipient count
 * (>= shown_count). name_of writes a short display name for an address.
 * Returns characters written. */
typedef void (*chat_receipt_name_fn)(char* out, size_t out_len, uint32_t addr);
int chat_format_receipt_summary(char* out, size_t out_len, const uint32_t* addrs,
                                size_t shown_count, size_t total, chat_receipt_name_fn name_of);

/* Compact age for message bubbles: "now", "5m", "3h", "2d". */
int chat_format_age(uint32_t age_s, char* buf, size_t buf_len);

#endif
