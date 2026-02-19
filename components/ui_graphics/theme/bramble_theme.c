#include "bramble_theme.h"

void bramble_theme_init(lv_display_t *disp) {
    /* Initialize LVGL's default dark theme */
    lv_theme_default_init(disp,
        BR_COLOR_PRIMARY, BR_COLOR_ACCENT,
        true,  /* dark mode */
        &lv_font_montserrat_14);

    /* Set the screen background color directly */
    lv_obj_set_style_bg_color(lv_screen_active(), BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_screen_active(), BR_COLOR_TEXT, 0);
}
