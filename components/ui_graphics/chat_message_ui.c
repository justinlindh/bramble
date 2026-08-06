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

bool chat_message_has_details_toggle(bool is_outgoing, uint32_t packet_id) {
    return is_outgoing && packet_id != 0;
}

int chat_format_receipt_summary(char* out, size_t out_len, const uint32_t* addrs,
                                size_t shown_count, size_t total, chat_receipt_name_fn name_of) {
    if (!out || out_len == 0)
        return 0;
    out[0] = '\0';
    if (!addrs || shown_count == 0 || total == 0 || !name_of)
        return snprintf(out, out_len, "No receipts yet");
    if (total < shown_count)
        total = shown_count;

    size_t pos = (size_t)snprintf(out, out_len, "Delivered to %u: ", (unsigned)total);
    for (size_t i = 0; i < shown_count && pos < out_len; i++) {
        char name[8];
        name_of(name, sizeof(name), addrs[i]);
        int n = snprintf(out + pos, out_len - pos, "%s%s", i ? ", " : "", name);
        if (n < 0 || (size_t)n >= out_len - pos)
            return (int)pos;
        pos += (size_t)n;
    }
    if (total > shown_count && pos < out_len) {
        int n = snprintf(out + pos, out_len - pos, ", +%u", (unsigned)(total - shown_count));
        if (n > 0 && (size_t)n < out_len - pos)
            pos += (size_t)n;
    }
    return (int)pos;
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
