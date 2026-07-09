#include "chat_target.h"

static bool is_broadcast_msg(const stored_msg_t* msg) {
    if (!msg)
        return false;
    return msg->direction == MSG_DIR_BROADCAST_IN || msg->direction == MSG_DIR_BROADCAST_OUT;
}

static bool is_dm_msg(const stored_msg_t* msg) {
    if (!msg)
        return false;
    return msg->direction == MSG_DIR_INCOMING || msg->direction == MSG_DIR_OUTGOING;
}

chat_target_t chat_target_default(void) {
    chat_target_t t = {
        .kind = CHAT_TARGET_BROADCAST,
        .channel_index = -1,
        .peer_addr = 0,
    };
    return t;
}

chat_target_t chat_target_normalize(chat_target_kind_t kind, int channel_index, int channel_count) {
    if (kind != CHAT_TARGET_CHANNEL) {
        return chat_target_default();
    }

    if (channel_count <= 1 || channel_index <= 0 || channel_index >= channel_count) {
        return chat_target_default();
    }

    chat_target_t t = {
        .kind = CHAT_TARGET_CHANNEL,
        .channel_index = (int16_t)channel_index,
        .peer_addr = 0,
    };
    return t;
}

chat_target_t chat_target_dm(uint32_t peer_addr) {
    chat_target_t t = {
        .kind = CHAT_TARGET_DM,
        .channel_index = -1,
        .peer_addr = peer_addr,
    };
    return t;
}

bool chat_target_matches_message(chat_target_t target, const stored_msg_t* msg,
                                 int message_channel_index) {
    if (!msg)
        return false;

    if (target.kind == CHAT_TARGET_BROADCAST) {
        return is_broadcast_msg(msg);
    }

    if (target.kind == CHAT_TARGET_CHANNEL) {
        return message_channel_index >= 0 && message_channel_index == target.channel_index;
    }

    if (target.kind == CHAT_TARGET_DM) {
        /* Channel traffic is also stored with INCOMING/OUTGOING directions;
         * a real DM carries no channel index. */
        return is_dm_msg(msg) && message_channel_index < 0 && msg->peer_addr == target.peer_addr;
    }

    return false;
}

chat_target_t chat_target_cycle(chat_target_t current, int channel_count) {
    if (current.kind == CHAT_TARGET_DM) {
        return chat_target_default();
    }

    if (channel_count <= 1) {
        return chat_target_default();
    }

    if (current.kind == CHAT_TARGET_BROADCAST) {
        return chat_target_normalize(CHAT_TARGET_CHANNEL, 1, channel_count);
    }

    int next = current.channel_index + 1;
    if (next >= channel_count) {
        return chat_target_default();
    }

    return chat_target_normalize(CHAT_TARGET_CHANNEL, next, channel_count);
}
