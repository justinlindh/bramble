#ifndef SCR_MAP_H
#define SCR_MAP_H

#include "scr_layout.h"

void scr_map_create(bramble_layout_t* layout);
void scr_map_set_focus_peer(uint32_t peer_addr);

/* Clear the focused peer (used when the user opens the Map tab directly). */
void scr_map_clear_focus_peer(void);

#endif
