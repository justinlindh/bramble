#ifndef SCR_LAYOUT_H
#define SCR_LAYOUT_H

#include "lvgl.h"

typedef enum {
    TAB_CHAT = 0,
    TAB_NODES,
    TAB_STATS,
    TAB_SETTINGS,
    TAB_COUNT
} bramble_tab_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *status_bar;
    lv_obj_t *content_area;
    lv_obj_t *tab_bar;
    lv_obj_t *tab_btns[TAB_COUNT];
    bramble_tab_t active_tab;

    /* Status bar labels */
    lv_obj_t *lbl_battery;
    lv_obj_t *lbl_signal;
    lv_obj_t *lbl_gps;
    lv_obj_t *lbl_time;
    lv_obj_t *lbl_node_name;

    /* Unread badge on chat tab */
    lv_obj_t *chat_badge;
} bramble_layout_t;

bramble_layout_t *layout_create(void);
void layout_set_tab(bramble_layout_t *layout, bramble_tab_t tab);
void layout_update_status(bramble_layout_t *layout);
void layout_set_unread(bramble_layout_t *layout, int count);
lv_obj_t *layout_get_content(bramble_layout_t *layout);

#endif
