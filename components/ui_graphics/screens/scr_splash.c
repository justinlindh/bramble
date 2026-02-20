#include "scr_splash.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"

/* External logo asset — stored in flash (.rodata), NOT in RAM */
extern const lv_image_dsc_t img_bramble_logo;

void scr_splash_create(lv_display_t *disp) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, BR_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /*
     * Vertical flex container — centers logo + title + subtitle as a unit.
     * Using a container avoids manual pixel math and keeps the group
     * visually centered regardless of font metrics.
     */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_remove_style_all(cont);                      /* transparent, no border/padding */
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont,
        LV_FLEX_ALIGN_CENTER,   /* main axis (column): space between items */
        LV_FLEX_ALIGN_CENTER,   /* cross axis: center items horizontally */
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 10, 0);              /* 10px gap between children */
    lv_obj_center(cont);                                /* center the whole group on screen */

    /* Logo image — 100×100, bramble-logo.png from webapp */
    lv_obj_t *logo = lv_image_create(cont);
    lv_image_set_src(logo, &img_bramble_logo);

    /* "BRAMBLE" text */
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text_static(title, "BRAMBLE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_PRIMARY, 0);

    /* "LoRa Mesh" subtitle */
    lv_obj_t *subtitle = lv_label_create(cont);
    lv_label_set_text_static(subtitle, "LoRa Mesh");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(subtitle, BR_COLOR_TEXT_SEC, 0);

    lv_screen_load(scr);
}
