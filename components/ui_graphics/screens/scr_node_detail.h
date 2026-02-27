#ifndef SCR_NODE_DETAIL_H
#define SCR_NODE_DETAIL_H

#include "scr_layout.h"
#include "routing.h"
#include "location.h"
#include <stdbool.h>
#include <stdint.h>

void scr_node_detail_open(bramble_layout_t *layout,
                          const neighbor_entry_t *neighbor,
                          bool has_location,
                          const location_cache_entry_t *location,
                          uint32_t now_ms);

#endif
