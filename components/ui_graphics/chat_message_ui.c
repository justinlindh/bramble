#include "chat_message_ui.h"

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

bool chat_message_has_inline_route_toggle(bool is_outgoing,
                                          msg_status_t status,
                                          uint8_t route_hop_count,
                                          uint32_t packet_id) {
    return is_outgoing &&
           status == MSG_STATUS_DELIVERED &&
           route_hop_count > 1 &&
           packet_id != 0;
}
