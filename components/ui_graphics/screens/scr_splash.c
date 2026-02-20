#include "scr_splash.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"

/* External logo asset */
extern const lv_image_dsc_t img_bramble_logo;

void scr_splash_create(lv_display_t *disp) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, BR_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    
    /* Logo image — 80×80, centered, slightly above middle */
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &img_bramble_logo);
    lv_obj_center(logo);
    lv_obj_set_y(logo, lv_pct(35));  /* slightly above center */
    
    /* "BRAMBLE" text — primary color */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_static(title, "BRAMBLE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_PRIMARY, 0);
    lv_obj_align_to(title, logo, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    
    /* "LoRa Mesh" text — secondary color */
    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text_static(subtitle, "LoRa Mesh");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(subtitle, BR_COLOR_TEXT_SEC, 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    
    lv_screen_load(scr);
}
