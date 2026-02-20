#include "scr_settings.h"
#include "theme/bramble_theme.h"
#include "display.h"
#include "keyboard.h"
#include "audio.h"
#include "board_config.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdio.h>

static const char *TAG = "scr_settings";

/* Get/set node name from mesh — extern since it's in main */
extern int mesh_set_node_name_persist(const char *name);
extern const char *mesh_get_node_name(void);

/* ── Backlight ───────────────────────────────────────────────────────── */

static void backlight_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);  /* 0..100 */
    /* Map slider range 0-100 → PWM brightness 0-255.
     * The keyboard MCU accepts the full 0-255 range; if it only supports
     * on/off, any value >0 will enable the backlight. */
    uint8_t brightness = (uint8_t)(val * 255 / 100);
    keyboard_set_backlight(brightness);
}

/* ── Volume ──────────────────────────────────────────────────────────── */

static lv_obj_t *s_volume_slider = NULL;

static void volume_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);  /* 0..100 */
    audio_set_volume((uint8_t)val);
}

static void volume_released_cb(lv_event_t *e) {
    (void)e;
    /* Play a short confirmation tone at the new volume so the user can hear
     * the result immediately.  Skip if muted — that's expected silence. */
    if (!audio_get_muted()) {
        audio_play_beep(880, 80);
    }
}

/* ── Silent mode ─────────────────────────────────────────────────────── */

static lv_obj_t *s_mute_sw = NULL;

static void mute_changed_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool muted = lv_obj_has_state(sw, LV_STATE_CHECKED);
    audio_set_muted(muted);

    /* Dim the volume slider when muted to hint it's inactive */
    if (s_volume_slider) {
        lv_obj_set_style_opa(s_volume_slider, muted ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}

/* ── Connectivity mode toggle ────────────────────────────────────────── */

/* Store dropdown reference so the apply button can read it */
static lv_obj_t *s_conn_dropdown = NULL;

static void conn_apply_cb(lv_event_t *e) {
    (void)e;
    if (!s_conn_dropdown) return;

    uint16_t sel = lv_dropdown_get_selected(s_conn_dropdown);
    conn_mode_t new_mode = (conn_mode_t)sel;
    conn_mode_t cur_mode = conn_mode_get();

    if (new_mode == cur_mode) {
        ESP_LOGI(TAG, "Connectivity mode unchanged (%d) — skipping reboot", (int)new_mode);
        return;
    }

    /* Persist before reboot */
    conn_mode_set(new_mode);
    ESP_LOGW(TAG, "Connectivity mode set to %d — rebooting...", (int)new_mode);
    esp_restart();
}

/* ── Reboot ──────────────────────────────────────────────────────────── */

static void reboot_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGW(TAG, "Rebooting by user request...");
    esp_restart();
}

/* ── Helper: labeled setting row ─────────────────────────────────────── */

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

/* ── Node Name edit modal ────────────────────────────────────────────── */

static lv_obj_t *s_name_label = NULL;
static lv_obj_t *s_name_edit_overlay = NULL;
static lv_obj_t *s_name_edit_ta = NULL;

static void name_edit_close(void) {
    if (!s_name_edit_overlay) return;
    lv_obj_delete(s_name_edit_overlay);
    s_name_edit_overlay = NULL;
    s_name_edit_ta = NULL;
}

static void name_edit_close_cb(lv_event_t *e) {
    (void)e;
    name_edit_close();
}

static void name_edit_save_cb(lv_event_t *e) {
    (void)e;
    if (!s_name_edit_ta) return;
    const char *new_name = lv_textarea_get_text(s_name_edit_ta);

    if (new_name && new_name[0] != '\0') {
        if (mesh_set_node_name_persist(new_name) == 0) {
            if (s_name_label) lv_label_set_text(s_name_label, new_name);
            ESP_LOGI(TAG, "Node name updated to: %s", new_name);
        } else {
            ESP_LOGW(TAG, "Failed to persist node name");
        }
    }

    name_edit_close();
}

static void name_edit_cb(lv_event_t *e) {
    (void)e;
    if (s_name_edit_overlay) return;

    lv_obj_t *scr = lv_screen_active();
    s_name_edit_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_name_edit_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_name_edit_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_name_edit_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_name_edit_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_name_edit_overlay, 0, 0);

    lv_obj_t *panel = lv_obj_create(s_name_edit_overlay);
    lv_obj_set_size(panel, 290, 132);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Set Node Name");
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    s_name_edit_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_name_edit_ta, 274, 36);
    lv_obj_align(s_name_edit_ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_max_length(s_name_edit_ta, 16);
    lv_textarea_set_one_line(s_name_edit_ta, true);
    const char *node_name = mesh_get_node_name();
    lv_textarea_set_text(s_name_edit_ta, node_name ? node_name : "");

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 128, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_add_event_cb(cancel_btn, name_edit_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t *save_btn = lv_btn_create(panel);
    lv_obj_set_size(save_btn, 128, 30);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(save_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_add_event_cb(save_btn, name_edit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);

    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_name_edit_ta);
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, save_btn);
        lv_group_focus_obj(s_name_edit_ta);
    }
}

/* ── Screen entry point ──────────────────────────────────────────────── */

void scr_settings_create(bramble_layout_t *layout) {
    lv_obj_t *cont = layout_get_content(layout);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, BR_PADDING, 0);
    lv_obj_set_style_pad_row(cont, 4, 0);

    lv_group_t *g = lv_group_get_default();

    /* ── Title ── */
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);

    /* ── Node Name (editable) ── */
    lv_obj_t *name_row = create_setting_row(cont, "Node Name");
    const char *node_name = mesh_get_node_name();
    s_name_label = lv_label_create(name_row);
    lv_label_set_text(s_name_label, node_name ? node_name : "(not set)");
    lv_obj_set_style_text_color(s_name_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_name_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(name_row, name_edit_cb, LV_EVENT_CLICKED, NULL);
    if (g) lv_group_add_obj(g, name_row);

    /* ── Keyboard Backlight slider ── */
    lv_obj_t *bl_row = create_setting_row(cont, "Backlight");
    lv_obj_set_size(bl_row, 304, 48);
    lv_obj_t *bl_slider = lv_slider_create(bl_row);
    lv_obj_set_size(bl_slider, 140, 10);
    lv_obj_align(bl_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(bl_slider, 0, 100);
    lv_slider_set_value(bl_slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bl_slider, lv_color_hex(0x333344), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(bl_slider, backlight_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, bl_slider);
    /* Apply initial brightness — LV_EVENT_VALUE_CHANGED doesn't fire at creation */
    keyboard_set_backlight(80 * 255 / 100);

    /* ── Volume slider ── */
    uint8_t cur_vol   = audio_get_volume();
    bool    cur_muted = audio_get_muted();

    lv_obj_t *vol_row = create_setting_row(cont, LV_SYMBOL_AUDIO " Volume");
    lv_obj_set_size(vol_row, 304, 48);
    s_volume_slider = lv_slider_create(vol_row);
    lv_obj_set_size(s_volume_slider, 140, 10);
    lv_obj_align(s_volume_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_slider_set_value(s_volume_slider, cur_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_volume_slider, lv_color_hex(0x333344), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_volume_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_volume_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_volume_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume_slider, volume_released_cb, LV_EVENT_RELEASED, NULL);
    /* Dim the slider when already muted to hint it's inactive */
    if (cur_muted) {
        lv_obj_set_style_opa(s_volume_slider, LV_OPA_40, 0);
    }
    if (g) lv_group_add_obj(g, s_volume_slider);

    /* ── Silent mode toggle ── */
    lv_obj_t *mute_row = create_setting_row(cont, LV_SYMBOL_MUTE " Silent");
    s_mute_sw = lv_switch_create(mute_row);
    lv_obj_align(s_mute_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_mute_sw, lv_color_hex(0x333344), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    /* Reflect saved mute state */
    if (cur_muted) {
        lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_mute_sw, mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, s_mute_sw);

    /* ── Connectivity Mode ── */
    {
        lv_obj_t *sep_conn = lv_obj_create(cont);
        lv_obj_set_size(sep_conn, 296, 1);
        lv_obj_set_style_bg_color(sep_conn, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_bg_opa(sep_conn, LV_OPA_30, 0);
        lv_obj_set_style_border_width(sep_conn, 0, 0);

        lv_obj_t *conn_section_lbl = lv_label_create(cont);
        lv_label_set_text(conn_section_lbl, LV_SYMBOL_WIFI " Connectivity Mode");
        lv_obj_set_style_text_font(conn_section_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(conn_section_lbl, BR_COLOR_TEXT, 0);

        /* Dropdown selector row */
        lv_obj_t *conn_row = create_setting_row(cont, "Mode");
        lv_obj_set_size(conn_row, 304, 48);

        s_conn_dropdown = lv_dropdown_create(conn_row);
        lv_dropdown_set_options(s_conn_dropdown,
            "WiFi only\n"
            "BLE only\n"
            "WiFi + BLE");
        lv_obj_set_size(s_conn_dropdown, 150, 34);
        lv_obj_align(s_conn_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

        /* Style to match dark theme */
        lv_obj_set_style_bg_color(s_conn_dropdown, lv_color_hex(0x1E1E3A), 0);
        lv_obj_set_style_text_color(s_conn_dropdown, BR_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(s_conn_dropdown, &lv_font_montserrat_12, 0);
        lv_obj_set_style_border_color(s_conn_dropdown, BR_COLOR_PRIMARY, 0);
        lv_obj_set_style_border_width(s_conn_dropdown, 1, 0);

        lv_obj_t *dd_list = lv_dropdown_get_list(s_conn_dropdown);
        lv_obj_set_style_bg_color(dd_list, lv_color_hex(0x1E1E3A), 0);
        lv_obj_set_style_text_color(dd_list, BR_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_12, 0);
        lv_obj_set_style_border_color(dd_list, BR_COLOR_PRIMARY, 0);

        /* Pre-select the currently persisted mode */
        conn_mode_t cur_conn = conn_mode_get();
        lv_dropdown_set_selected(s_conn_dropdown, (uint16_t)cur_conn);

        if (g) lv_group_add_obj(g, s_conn_dropdown);

        /* Helper hint text */
        lv_obj_t *conn_hint = lv_label_create(cont);
        lv_label_set_text(conn_hint,
            "WiFi: WebSocket RPC + OTA updates\n"
            "BLE: Bluetooth GATT RPC\n"
            "WiFi+BLE: Both (uses more RAM)");
        lv_obj_set_style_text_font(conn_hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(conn_hint, BR_COLOR_TEXT_SEC, 0);

        /* Apply & Reboot button */
        lv_obj_t *apply_btn = lv_btn_create(cont);
        lv_obj_set_size(apply_btn, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(apply_btn, BR_COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0x0A6B60), LV_STATE_PRESSED);
        lv_obj_set_style_radius(apply_btn, BR_RADIUS, 0);
        lv_obj_t *apply_lbl = lv_label_create(apply_btn);
        lv_label_set_text(apply_lbl, LV_SYMBOL_OK " Apply Mode & Reboot");
        lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(apply_lbl);
        lv_obj_add_event_cb(apply_btn, conn_apply_cb, LV_EVENT_CLICKED, NULL);
        if (g) lv_group_add_obj(g, apply_btn);
    }

    /* ── Board info ── */
    lv_obj_t *board_row = create_setting_row(cont, "Board");
    const bramble_board_config_t *board = board_get_config();
    lv_obj_t *board_val = lv_label_create(board_row);
    lv_label_set_text(board_val, board->name);
    lv_obj_set_style_text_color(board_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(board_val, &lv_font_montserrat_12, 0);
    lv_obj_align(board_val, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ── Separator ── */
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* ── Version ── */
    lv_obj_t *ver_row = create_setting_row(cont, "Version");
    lv_obj_t *ver_val = lv_label_create(ver_row);
    lv_label_set_text(ver_val, "0.9.1-tdeck");
    lv_obj_set_style_text_color(ver_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(ver_val, &lv_font_montserrat_12, 0);
    lv_obj_align(ver_val, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ── Reboot button ── */
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
