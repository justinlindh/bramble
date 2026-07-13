#ifndef SCR_CHAT_MESSAGES_H
#define SCR_CHAT_MESSAGES_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include "chat_target.h"
#include "scr_layout.h"

void scr_chat_messages_open(bramble_layout_t* layout, int channel_idx);
void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr);
void scr_chat_messages_on_recv(void);

/* Which conversation is on screen right now, if any: true (and *out is the
 * open target) exactly while a chat thread is built, false on every other
 * screen. The authoritative answer for "should an arriving message repaint the
 * open view" - the nav tab is not, since a DM opened from node detail leaves
 * active_tab == TAB_NODES. Cleared automatically on teardown. */
bool scr_chat_messages_open_target(chat_target_t* out);

#endif
