#include "chat_message_ui.h"
#include <stdio.h>

chat_delivery_badge_t chat_message_delivery_badge(msg_status_t status) {
    switch (status) {
    case MSG_STATUS_SENT:
        return (chat_delivery_badge_t){
            .kind = CHAT_DELIVERY_BADGE_SINGLE_CHECK,
            .color_role = CHAT_DELIVERY_COLOR_UNDELIVERED,
        };
    case MSG_STATUS_DELIVERED:
        return (chat_delivery_badge_t){
            .kind = CHAT_DELIVERY_BADGE_DOUBLE_CHECK,
            .color_role = CHAT_DELIVERY_COLOR_DELIVERED,
        };
    case MSG_STATUS_FAILED:
        return (chat_delivery_badge_t){
            .kind = CHAT_DELIVERY_BADGE_FAILED,
            .color_role = CHAT_DELIVERY_COLOR_FAILED,
        };
    default:
        return (chat_delivery_badge_t){
            .kind = CHAT_DELIVERY_BADGE_PENDING,
            .color_role = CHAT_DELIVERY_COLOR_UNDELIVERED,
        };
    }
}

bool chat_message_has_inline_route_toggle(bool is_outgoing, msg_status_t status,
                                          uint8_t route_hop_count, uint32_t packet_id) {
    return is_outgoing && status == MSG_STATUS_DELIVERED && route_hop_count > 1 && packet_id != 0;
}

int chat_format_age(uint32_t age_s, char* buf, size_t buf_len) {
    if (!buf || buf_len == 0)
        return 0;
    if (age_s < 60)
        return snprintf(buf, buf_len, "now");
    if (age_s < 3600)
        return snprintf(buf, buf_len, "%um", (unsigned)(age_s / 60));
    if (age_s < 86400)
        return snprintf(buf, buf_len, "%uh", (unsigned)(age_s / 3600));
    return snprintf(buf, buf_len, "%ud", (unsigned)(age_s / 86400));
}
