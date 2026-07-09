#include "ui_toast.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"

#define TOAST_DURATION_MS 2500

static lv_obj_t* s_toast = NULL;
static lv_timer_t* s_timer = NULL;

static void toast_dismiss(void) {
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

static void toast_timer_cb(lv_timer_t* timer) {
    (void)timer;
    s_timer = NULL; /* one-shot: LVGL deletes it after repeat_count hits 0 */
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

void ui_toast_show(const char* text) {
    if (!text || !text[0])
        return;

    toast_dismiss();

    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_radius(s_toast, BR_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_toast, 10, 0);
    lv_obj_set_style_pad_ver(s_toast, 6, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -48);

    lv_obj_t* lbl = lv_label_create(s_toast);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(lbl, 280, 0);

    s_timer = lv_timer_create(toast_timer_cb, TOAST_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}
