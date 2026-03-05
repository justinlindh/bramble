#include "chat_unread.h"
#include <string.h>

#define CHAT_UNREAD_MAX_CHANNELS 32

static int s_unread_counts[CHAT_UNREAD_MAX_CHANNELS];

static int normalize_channel_for_unread(const stored_msg_t* msg) {
    if (!msg)
        return -1;

    if (msg->direction == MSG_DIR_BROADCAST_IN) {
        return 0;
    }

    if (msg->direction == MSG_DIR_INCOMING) {
        return msg->channel_index;
    }

    return -1;
}

void chat_unread_mark_for_message(const stored_msg_t* msg) {
    int channel = normalize_channel_for_unread(msg);
    if (channel < 0 || channel >= CHAT_UNREAD_MAX_CHANNELS)
        return;
    s_unread_counts[channel]++;
}

int chat_unread_count_for_channel(int channel_idx) {
    if (channel_idx < 0 || channel_idx >= CHAT_UNREAD_MAX_CHANNELS)
        return 0;
    return s_unread_counts[channel_idx];
}

void chat_unread_clear_for_channel(int channel_idx) {
    if (channel_idx < 0 || channel_idx >= CHAT_UNREAD_MAX_CHANNELS)
        return;
    s_unread_counts[channel_idx] = 0;
}

void chat_unread_reset(void) { memset(s_unread_counts, 0, sizeof(s_unread_counts)); }
