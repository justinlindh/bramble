#ifndef SCR_LAYOUT_H
#define SCR_LAYOUT_H

#include "lvgl.h"
#include <stdbool.h>

typedef enum { TAB_CHAT = 0, TAB_NODES, TAB_MAP, TAB_STATS, TAB_SETTINGS, TAB_COUNT } bramble_tab_t;

typedef struct {
    lv_obj_t* screen;
    lv_obj_t* status_bar;
    lv_obj_t* content_area;
    lv_obj_t* tab_bar;
    lv_obj_t* tab_btns[TAB_COUNT];
    bramble_tab_t active_tab;

    /* Status bar labels */
    lv_obj_t* lbl_battery;
    lv_obj_t* lbl_signal;
    lv_obj_t* lbl_gps;
    lv_obj_t* lbl_time;
    lv_obj_t* lbl_node_name;

    /* Unread badge on chat tab */
    lv_obj_t* chat_badge;
} bramble_layout_t;

bramble_layout_t* layout_create(void);
void layout_set_tab(bramble_layout_t* layout, bramble_tab_t tab);

/* THE way to swap the content area to a new screen: clean the content area, run
 * the builder to repopulate it, then reset the focus zone to content. Every
 * full-screen transition (tab dispatch, back buttons, chat/SAS/compose/channel
 * entries) goes through here so no screen builder can forget the zone reset -
 * forgetting it in scr_chat_list_create was bug F2 (a content row lit green
 * while input stayed in chrome, so SELECT fired a nav tab and jumped screens).
 *
 * The builder receives the layout and an opaque ctx; builders that take other
 * argument shapes get a tiny adapter. Deferred (ui_defer'd) transitions must
 * defer the WHOLE call to this, not just the builder, so the clean happens once
 * event dispatch has unwound. Callers that need pending layout flushed before
 * the clean (flex/content-size screens) call lv_refr_now() before this. */
void layout_rebuild_content(bramble_layout_t* layout, void (*builder)(bramble_layout_t*, void*),
                            void* ctx);
void layout_update_status(bramble_layout_t* layout);
void layout_set_unread(bramble_layout_t* layout, int count);
lv_obj_t* layout_get_content(bramble_layout_t* layout);

/* Hide/show the tab bar AND remove/re-add its buttons from the input
 * group, so hidden tabs cannot be focused by keyboard/trackball. */
void layout_set_tab_bar_hidden(bramble_layout_t* layout, bool hidden);

/* Move the nav tabs to the END of the chrome focus ring. Call at the end of any
 * builder that registers header actions in the chrome group while the tab bar
 * is visible, so a content->chrome hop lands on the screen's primary action
 * instead of walking the whole five-tab strip to reach it. */
void layout_chrome_tabs_last(bramble_layout_t* layout);

#endif
