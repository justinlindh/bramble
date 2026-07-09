#include "ui_confirm.h"
#include "ui_focus.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"

static lv_obj_t* s_overlay = NULL;
static ui_confirm_cb_t s_on_confirm = NULL;
static void* s_user_data = NULL;

static void confirm_close(void) {
    if (s_overlay) {
        ui_focus_pop_modal();
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
}

static void cancel_cb(lv_event_t* e) {
    (void)e;
    confirm_close();
}

static void confirm_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_cb_t cb = s_on_confirm;
    void* ud = s_user_data;
    confirm_close();
    if (cb)
        cb(ud);
}

void ui_confirm_show(const char* text, const char* confirm_label, ui_confirm_cb_t on_confirm,
                     void* user_data) {
    if (!text || !confirm_label)
        return;

    confirm_close();
    ui_focus_push_modal();
    s_on_confirm = on_confirm;
    s_user_data = user_data;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_overlay);
    lv_obj_set_size(panel, 260, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(panel, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* msg = lv_label_create(panel);
    lv_label_set_text(msg, text);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));

    lv_obj_t* btn_row = lv_obj_create(panel);
    lv_obj_set_size(btn_row, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_group_t* g = ui_focus_active_group();

    lv_obj_t* cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 110, 32);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t* ok_btn = lv_btn_create(btn_row);
    lv_obj_set_size(ok_btn, 110, 32);
    lv_obj_align(ok_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ok_btn, BR_COLOR_DANGER, 0);
    lv_obj_add_event_cb(ok_btn, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, confirm_label);
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(ok_lbl);

    if (g) {
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, ok_btn);
        lv_group_focus_obj(cancel_btn);
    }
}
