#include "chat_unread.h"
#include <stdbool.h>
#include <string.h>

#define CHAT_UNREAD_MAX_CHANNELS 32
/* Must be >= the Messages list's DM_PEERS_MAX (scr_chat_list.c): the list
 * renders up to that many DM rows, and a peer with no tracked slot here can
 * never show its per-row unread badge. */
#define CHAT_UNREAD_MAX_DMS 12

static int s_unread_counts[CHAT_UNREAD_MAX_CHANNELS];

typedef struct {
    uint32_t peer_addr; /* 0 = free slot */
    int count;
} dm_unread_t;

static dm_unread_t s_dm_unread[CHAT_UNREAD_MAX_DMS];

static dm_unread_t* dm_slot(uint32_t peer_addr, bool create) {
    dm_unread_t* free_slot = NULL;
    for (int i = 0; i < CHAT_UNREAD_MAX_DMS; i++) {
        if (s_dm_unread[i].peer_addr == peer_addr)
            return &s_dm_unread[i];
        if (!s_dm_unread[i].peer_addr && !free_slot)
            free_slot = &s_dm_unread[i];
    }
    if (create && free_slot) {
        free_slot->peer_addr = peer_addr;
        free_slot->count = 0;
        return free_slot;
    }
    return NULL;
}

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
    if (!msg)
        return;

    if (msg->direction == MSG_DIR_INCOMING && msg->channel_index < 0 && msg->peer_addr != 0) {
        dm_unread_t* slot = dm_slot(msg->peer_addr, true);
        if (slot)
            slot->count++;
        return;
    }

    int channel = normalize_channel_for_unread(msg);
    if (channel < 0 || channel >= CHAT_UNREAD_MAX_CHANNELS)
        return;
    s_unread_counts[channel]++;
}

int chat_unread_count_for_dm(uint32_t peer_addr) {
    dm_unread_t* slot = dm_slot(peer_addr, false);
    return slot ? slot->count : 0;
}

void chat_unread_clear_for_dm(uint32_t peer_addr) {
    dm_unread_t* slot = dm_slot(peer_addr, false);
    if (slot) {
        slot->peer_addr = 0;
        slot->count = 0;
    }
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

void chat_unread_reset(void) {
    memset(s_unread_counts, 0, sizeof(s_unread_counts));
    memset(s_dm_unread, 0, sizeof(s_dm_unread));
}
