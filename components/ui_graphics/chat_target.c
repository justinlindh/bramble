#include "chat_target.h"

static bool is_broadcast_msg(const stored_msg_t *msg) {
    if (!msg) return false;
    return msg->direction == MSG_DIR_BROADCAST_IN ||
           msg->direction == MSG_DIR_BROADCAST_OUT;
}

chat_target_t chat_target_default(void) {
    chat_target_t t = {
        .kind = CHAT_TARGET_BROADCAST,
        .channel_index = -1,
    };
    return t;
}

chat_target_t chat_target_normalize(chat_target_kind_t kind,
                                    int channel_index,
                                    int channel_count) {
    if (kind != CHAT_TARGET_CHANNEL) {
        return chat_target_default();
    }

    /* Channel 0 is Broadcast special-case; valid private channels are 1..count-1 */
    if (channel_count <= 1 || channel_index <= 0 || channel_index >= channel_count) {
        return chat_target_default();
    }

    chat_target_t t = {
        .kind = CHAT_TARGET_CHANNEL,
        .channel_index = (int16_t)channel_index,
    };
    return t;
}

bool chat_target_matches_message(chat_target_t target,
                                 const stored_msg_t *msg,
                                 int message_channel_index) {
    if (!msg) return false;

    if (target.kind == CHAT_TARGET_BROADCAST) {
        return is_broadcast_msg(msg);
    }

    if (is_broadcast_msg(msg)) {
        return false;
    }

    return message_channel_index >= 0 && message_channel_index == target.channel_index;
}
