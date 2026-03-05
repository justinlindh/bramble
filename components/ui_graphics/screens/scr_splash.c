#include "scr_splash.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"
#include "esp_app_desc.h"

/* External logo asset — stored in flash (.rodata), NOT in RAM */
extern const lv_image_dsc_t img_bramble_logo;

void scr_splash_create(lv_display_t* disp) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, BR_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /*
     * Vertical flex container — centers logo + title + subtitle + version
     * as a unit. Avoids manual pixel math.
     */
    lv_obj_t* cont = lv_obj_create(scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, /* main axis: center children vertically */
                          LV_FLEX_ALIGN_CENTER,       /* cross axis: center children horizontally */
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_center(cont);

    /* Logo image — 100×100 */
    lv_obj_t* logo = lv_image_create(cont);
    lv_image_set_src(logo, &img_bramble_logo);

    /* "BRAMBLE" */
    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text_static(title, "BRAMBLE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_PRIMARY, 0);

    /* "LoRa Mesh" */
    lv_obj_t* subtitle = lv_label_create(cont);
    lv_label_set_text_static(subtitle, "LoRa Mesh");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(subtitle, BR_COLOR_TEXT_SEC, 0);

    /* Version string from app binary metadata (set via PROJECT_VER in CMakeLists) */
    const esp_app_desc_t* app_desc = esp_app_get_description();
    static char ver_buf[36]; /* esp_app_desc_t.version is 32 bytes max */
    snprintf(ver_buf, sizeof(ver_buf), "v%s", app_desc->version);

    lv_obj_t* version = lv_label_create(cont);
    lv_label_set_text(version, ver_buf);
    lv_obj_set_style_text_font(version, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(version, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_opa(version, LV_OPA_50, 0); /* dim — secondary info */

    lv_screen_load(scr);
}
