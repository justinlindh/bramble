#include "ui_focus.h"

static lv_group_t* s_modal_group = NULL;

static void set_input_group(lv_group_t* g) {
    for (lv_indev_t* indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER)
            lv_indev_set_group(indev, g);
    }
}

void ui_focus_push_modal(void) {
    if (s_modal_group)
        ui_focus_pop_modal(); /* single-level: a new modal replaces the old */
    s_modal_group = lv_group_create();
    set_input_group(s_modal_group);
}

void ui_focus_pop_modal(void) {
    if (!s_modal_group)
        return;
    set_input_group(lv_group_get_default());
    lv_group_delete(s_modal_group);
    s_modal_group = NULL;
}

lv_group_t* ui_focus_active_group(void) {
    return s_modal_group ? s_modal_group : lv_group_get_default();
}
