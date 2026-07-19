#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_confirm.h"
#include "ui_toast.h"
#include "ui_zone.h"
#include "ui.h"
#include "wifi_manager.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>

static const char* TAG = "scr_set_conn";

/* Store dropdown reference so the apply button can read it */
static lv_obj_t* s_conn_dropdown = NULL;

static void do_apply_conn_mode(void* user_data) {
    (void)user_data;
    if (!s_conn_dropdown)
        return;
    conn_mode_t new_mode = (conn_mode_t)lv_dropdown_get_selected(s_conn_dropdown);
    conn_mode_set(new_mode);
    ESP_LOGW(TAG, "Connectivity mode set to %d, rebooting...", (int)new_mode);
    esp_restart();
}

static void conn_apply_cb(lv_event_t* e) {
    (void)e;
    if (!s_conn_dropdown)
        return;

    conn_mode_t new_mode = (conn_mode_t)lv_dropdown_get_selected(s_conn_dropdown);
    if (new_mode == conn_mode_get()) {
        ui_toast_show("Mode unchanged");
        return;
    }
    ui_confirm_show("Switch connectivity mode and reboot?", "Apply", do_apply_conn_mode, NULL);
}

/* ── Subpage ─────────────────────────────────────────────────────────────── */

void settings_connectivity_summary(char* buf, size_t n) {
    snprintf(buf, n, "%s", conn_mode_get() == CONN_MODE_WIFI ? "WiFi" : "BLE");
}

void settings_connectivity_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_t* cont = settings_subpage_begin(layout, "Connectivity");
    lv_group_t* g = lv_group_get_default();

    /* Dropdown selector row */
    lv_obj_t* conn_row = settings_create_setting_row(cont, "Mode");
    lv_obj_set_size(conn_row, 304, 48);

    ui_zone_track(&s_conn_dropdown, lv_dropdown_create(conn_row));
    lv_dropdown_set_options(s_conn_dropdown, "WiFi only\n"
                                             "BLE only");
    lv_obj_set_size(s_conn_dropdown, 150, 34);
    lv_obj_align(s_conn_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Style to match dark theme */
    lv_obj_set_style_bg_color(s_conn_dropdown, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_color(s_conn_dropdown, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_conn_dropdown, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_color(s_conn_dropdown, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_conn_dropdown, 1, 0);

    lv_obj_t* dd_list = lv_dropdown_get_list(s_conn_dropdown);
    lv_obj_set_style_bg_color(dd_list, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_color(dd_list, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_color(dd_list, BR_COLOR_PRIMARY, 0);

    /* Pre-select the currently persisted mode */
    conn_mode_t cur_conn = conn_mode_get();
    lv_dropdown_set_selected(s_conn_dropdown, (uint16_t)cur_conn);

    if (g)
        lv_group_add_obj(g, s_conn_dropdown);

    /* Helper hint text */
    lv_obj_t* conn_hint = lv_label_create(cont);
    lv_label_set_text(conn_hint, "WiFi: WebSocket RPC + OTA updates\n"
                                 "BLE: Bluetooth GATT RPC\n"
                                 "Modes are exclusive");
    lv_obj_set_style_text_font(conn_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(conn_hint, BR_COLOR_TEXT_SEC, 0);

    /* AP-mode credentials. The SoftAP password is derived per device, so
     * this screen is where a T-Deck user reads it off (issue #78). Shown
     * only while the AP is actually up: in station mode there is nothing to
     * join and nothing to display. */
    {
        wifi_status_t wst;
        wifi_manager_get_status(&wst);
        if (wst.mode == BRAMBLE_WIFI_AP && wst.ap_password[0] != '\0') {
            lv_obj_t* ap_lbl = lv_label_create(cont);
            char ap_text[192];
            snprintf(ap_text, sizeof(ap_text), "Access point: %s\nPassword: %s\nThen open %s",
                     wst.ssid, wst.ap_password, wst.ip_addr);
            lv_label_set_text(ap_lbl, ap_text);
            lv_obj_set_style_text_font(ap_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(ap_lbl, BR_COLOR_TEXT, 0);
        }
    }

    /* Apply & Reboot button */
    lv_obj_t* apply_btn = lv_btn_create(cont);
    lv_obj_set_size(apply_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(apply_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0x1A6628),
                              LV_STATE_PRESSED); /* Darker green */
    lv_obj_set_style_radius(apply_btn, BR_RADIUS, 0);
    lv_obj_t* apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, LV_SYMBOL_OK " Apply Mode & Reboot");
    lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(apply_lbl);
    lv_obj_add_event_cb(apply_btn, conn_apply_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, apply_btn);
}
