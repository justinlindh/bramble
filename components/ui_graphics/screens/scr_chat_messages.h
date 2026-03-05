#ifndef SCR_CHAT_MESSAGES_H
#define SCR_CHAT_MESSAGES_H

#include "lvgl.h"
#include <stdint.h>
#include "scr_layout.h"

void scr_chat_messages_open(bramble_layout_t* layout, int channel_idx);
void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr);
void scr_chat_messages_on_recv(void);

#endif
