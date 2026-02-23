#ifndef CHAT_UNREAD_H
#define CHAT_UNREAD_H

#include "msg_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void chat_unread_mark_for_message(const stored_msg_t *msg);
int chat_unread_count_for_channel(int channel_idx);
void chat_unread_clear_for_channel(int channel_idx);
void chat_unread_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_UNREAD_H */
