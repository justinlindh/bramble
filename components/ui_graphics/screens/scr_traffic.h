#ifndef SCR_TRAFFIC_H
#define SCR_TRAFFIC_H

#include "lvgl.h"
#include "scr_layout.h"

/**
 * Open the Traffic Monitor sub-screen.
 *
 * Shows a scrollable log of recent mesh packets with direction,
 * packet type, category, size, and RSSI. Accessible from the Stats tab.
 *
 * Call after lv_obj_clean(layout->content_area).
 */
void scr_traffic_create(bramble_layout_t *layout);

#endif /* SCR_TRAFFIC_H */
