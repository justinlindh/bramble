#include "scr_settings.h"
#include "theme/bramble_theme.h"
#include "display.h"
#include "keyboard.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdio.h>

static const char *TAG = "scr_settings";

/* Get node name from mesh — extern since it's in main */
extern void mesh_set_node_name(const char *name);

static void backlight_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    /* Keyboard backlight is binary (on/off via I2C to ESP32-C3 MCU).
     * Slider min is 10, so any value = on. Use 0 threshold for off. */
    keyboard_set_backlight(val > 0);
}

static void reboot_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGW(TAG, "Rebooting by user request...");
    esp_restart();
}

static lv_obj_t *create_setting_row(lv_obj_t *parent, const char *label) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, BR_RADIUS, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_FOCUSED);
    
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    return row;
}

void scr_settings_create(bramble_layout_t *layout) {
    lv_obj_t *cont = layout_get_content(layout);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, BR_PADDING, 0);
    lv_obj_set_style_pad_row(cont, 4, 0);
    
    lv_group_t *g = lv_group_get_default();
    
    /* Title */
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    
    /* Node Name (read-only for now) */
    lv_obj_t *name_row = create_setting_row(cont, "Node Name");
    const bramble_board_config_t *board = board_get_config();
    lv_obj_t *name_val = lv_label_create(name_row);
    lv_label_set_text(name_val, board->name);
    lv_obj_set_style_text_color(name_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(name_val, &lv_font_montserrat_12, 0);
    lv_obj_align(name_val, LV_ALIGN_RIGHT_MID, 0, 0);
    if (g) lv_group_add_obj(g, name_row);
    
    /* Backlight slider */
    lv_obj_t *bl_row = create_setting_row(cont, "Backlight");
    lv_obj_set_size(bl_row, 304, 48);
    lv_obj_t *slider = lv_slider_create(bl_row);
    lv_obj_set_size(slider, 140, 10);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333344), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, backlight_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, slider);
    
    /* Board info */
    lv_obj_t *board_row = create_setting_row(cont, "Board");
    lv_obj_t *board_val = lv_label_create(board_row);
    lv_label_set_text(board_val, board->name);
    lv_obj_set_style_text_color(board_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(board_val, &lv_font_montserrat_12, 0);
    lv_obj_align(board_val, LV_ALIGN_RIGHT_MID, 0, 0);
    
    /* Separator */
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    
    /* Version */
    lv_obj_t *ver_row = create_setting_row(cont, "Version");
    lv_obj_t *ver_val = lv_label_create(ver_row);
    lv_label_set_text(ver_val, "0.9.1-tdeck");  /* Hardcoded for now */
    lv_obj_set_style_text_color(ver_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(ver_val, &lv_font_montserrat_12, 0);
    lv_obj_align(ver_val, LV_ALIGN_RIGHT_MID, 0, 0);
    
    /* Reboot button */
    lv_obj_t *reboot_btn = lv_btn_create(cont);
    lv_obj_set_size(reboot_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(reboot_btn, BR_COLOR_DANGER, 0);
    lv_obj_set_style_radius(reboot_btn, BR_RADIUS, 0);
    lv_obj_t *reboot_lbl = lv_label_create(reboot_btn);
    lv_label_set_text(reboot_lbl, "Reboot Device");
    lv_obj_set_style_text_font(reboot_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(reboot_lbl);
    lv_obj_add_event_cb(reboot_btn, reboot_cb, LV_EVENT_CLICKED, NULL);
    if (g) lv_group_add_obj(g, reboot_btn);
}
