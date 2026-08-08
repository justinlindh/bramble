#ifndef SCR_NODE_DETAIL_H
#define SCR_NODE_DETAIL_H

#include "scr_layout.h"
#include "routing.h"
#include "location.h"
#include <stdbool.h>
#include <stdint.h>

/* The peer is all the caller supplies: the card reads its own clock, its own
 * neighbor entry and its own location-cache entry on every render, so passing
 * a location or a timestamp in would only fix the first frame to values the
 * next tick overwrites. */
void scr_node_detail_open(bramble_layout_t* layout, const neighbor_entry_t* neighbor);

#endif
