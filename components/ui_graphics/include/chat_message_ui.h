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
bool chat_message_has_inline_route_toggle(bool is_outgoing, msg_status_t status,
                                          uint8_t route_hop_count, uint32_t packet_id);

/* Compact age for message bubbles: "now", "5m", "3h", "2d". */
int chat_format_age(uint32_t age_s, char* buf, size_t buf_len);

#endif
