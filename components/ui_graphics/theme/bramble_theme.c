#include "bramble_theme.h"

static void theme_apply_cb(lv_theme_t *th, lv_obj_t *obj);

void bramble_theme_init(lv_display_t *disp) {
    /* Start from LVGL's default dark theme */
    lv_theme_t *parent = lv_theme_default_init(disp,
        BR_COLOR_PRIMARY, BR_COLOR_ACCENT,
        true,  /* dark mode */
        LV_FONT_DEFAULT);

    /* Override the apply callback to set our background color */
    static lv_theme_t theme;
    theme = *parent;
    theme.apply_cb = theme_apply_cb;
    lv_display_set_theme(disp, &theme);
}

static void theme_apply_cb(lv_theme_t *th, lv_obj_t *obj) {
    /* Apply base theme first */
    lv_theme_default_apply(obj);

    /* Override screen backgrounds to our dark color */
    if (lv_obj_get_parent(obj) == NULL) {
        lv_obj_set_style_bg_color(obj, BR_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(obj, BR_COLOR_TEXT, 0);
    }
}
