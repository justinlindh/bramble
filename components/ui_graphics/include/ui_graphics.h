#ifndef BRAMBLE_UI_GRAPHICS_H
#define BRAMBLE_UI_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

int ui_graphics_init(void);
uint32_t ui_graphics_tick(void);
void ui_graphics_notify(uint32_t event_mask);

#define UI_EVT_MSG_RECEIVED     (1 << 0)
#define UI_EVT_NODE_UPDATE      (1 << 1)
#define UI_EVT_STATS_UPDATE     (1 << 2)
#define UI_EVT_BATTERY_UPDATE   (1 << 3)
#define UI_EVT_GPS_UPDATE       (1 << 4)
#define UI_EVT_CONN_CHANGE      (1 << 5)

#endif
