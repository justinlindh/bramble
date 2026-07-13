#include "ui_focus.h"
#include "ui_zone.h"

static lv_group_t* s_modal_group = NULL;

void ui_focus_push_modal(void) {
    if (s_modal_group)
        ui_focus_pop_modal(); /* single-level: a new modal replaces the old */
    s_modal_group = lv_group_create();
    ui_zone_bind_indevs(s_modal_group);
}

void ui_focus_pop_modal(void) {
    if (!s_modal_group)
        return;
    lv_group_t* dead = s_modal_group;
    s_modal_group = NULL;
    /* Restore input to the content zone before deleting the modal's group, so
     * no indev is left pointing at a freed group. */
    ui_zone_reset_to_content();
    lv_group_delete(dead);
}

lv_group_t* ui_focus_active_group(void) {
    return s_modal_group ? s_modal_group : lv_group_get_default();
}

bool ui_focus_modal_active(void) { return s_modal_group != NULL; }
