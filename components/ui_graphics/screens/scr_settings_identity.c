#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_focus.h"
#include "ui_zone.h"
#include "ui_toast.h"
#include "board_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_set_id";

/* Get/set node name from mesh, plus identity: extern since they live in main. */
extern int mesh_set_node_name_persist(const char* name);
extern const char* mesh_get_node_name(void);
extern int mesh_get_identity(uint32_t* addr_out, uint8_t pubkey_out[32]);

/* ── Identity QR share modal ─────────────────────────────────────────────── */

static lv_obj_t* s_identity_qr_overlay = NULL;

static size_t base64url_encode_no_pad(const uint8_t* in, size_t in_len, char* out, size_t out_cap) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t out_len = 0;

    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t n = (a << 16) | (b << 8) | c;

        if (out_len + 2 >= out_cap)
            return 0;
        out[out_len++] = tbl[(n >> 18) & 0x3F];
        out[out_len++] = tbl[(n >> 12) & 0x3F];

        if (i + 1 < in_len) {
            if (out_len + 1 >= out_cap)
                return 0;
            out[out_len++] = tbl[(n >> 6) & 0x3F];
        }
        if (i + 2 < in_len) {
            if (out_len + 1 >= out_cap)
                return 0;
            out[out_len++] = tbl[n & 0x3F];
        }
    }

    if (out_len >= out_cap)
        return 0;
    out[out_len] = '\0';
    return out_len;
}

static void identity_qr_close(void) {
    if (!s_identity_qr_overlay)
        return;
    ui_focus_pop_modal();
    /* Called from the Close button INSIDE this overlay: a plain lv_obj_delete
     * would free the widget whose CLICKED event is still dispatching (LVGL 9
     * forbids it; symptom is a use-after-free reboot). Same pattern as
     * ui_confirm and the other settings modals. */
    lv_obj_delete_async(s_identity_qr_overlay);
    s_identity_qr_overlay = NULL;
}

static void identity_qr_close_cb(lv_event_t* e) {
    (void)e;
    identity_qr_close();
}

static void identity_qr_open_cb(lv_event_t* e) {
    (void)e;
    if (s_identity_qr_overlay)
        return;

    uint32_t addr = 0;
    uint8_t pubkey[32] = {0};
    if (mesh_get_identity(&addr, pubkey) != 0) {
        ESP_LOGW(TAG, "Identity unavailable for QR share");
        return;
    }

    const char* node_name = mesh_get_node_name();
    char name_sanitized[48];
    size_t ni = 0;
    if (node_name && node_name[0]) {
        for (size_t i = 0; node_name[i] != '\0' && ni < sizeof(name_sanitized) - 1; i++) {
            char ch = node_name[i];
            name_sanitized[ni++] = (ch == ' ') ? '_' : ch;
        }
    } else {
        const char fallback[] = "unnamed";
        memcpy(name_sanitized, fallback, sizeof(fallback));
        ni = sizeof(fallback) - 1;
    }
    name_sanitized[ni] = '\0';

    char pubkey_b64url[48];
    if (base64url_encode_no_pad(pubkey, sizeof(pubkey), pubkey_b64url, sizeof(pubkey_b64url)) ==
        0) {
        ESP_LOGW(TAG, "Failed to encode pubkey for QR share");
        return;
    }

    char share[192];
    snprintf(share, sizeof(share), "bramble://node/v1?n=%s&a=%08lX&pk=%s", name_sanitized,
             (unsigned long)addr, pubkey_b64url);

    ui_focus_push_modal();
    lv_obj_t* scr = lv_screen_active();
    s_identity_qr_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_identity_qr_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_identity_qr_overlay, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_identity_qr_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_identity_qr_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_identity_qr_overlay, 0, 0);

    lv_obj_t* title = lv_label_create(s_identity_qr_overlay);
    lv_label_set_text(title, "Share Node Identity");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* qr = lv_qrcode_create(s_identity_qr_overlay);
    lv_qrcode_set_size(qr, 260);
    lv_qrcode_set_dark_color(qr, BR_COLOR_TEXT);
    lv_qrcode_set_light_color(qr, BR_COLOR_BG);
    lv_result_t qr_res = lv_qrcode_update(qr, share, strlen(share));
    if (qr_res != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_qrcode_update failed: %d", (int)qr_res);
        identity_qr_close();
        return;
    }
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t* hint = lv_label_create(s_identity_qr_overlay);
    lv_label_set_text(hint, "Press Back to close");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_t* close_btn = lv_btn_create(s_identity_qr_overlay);
    lv_obj_set_size(close_btn, 96, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_event_cb(close_btn, identity_qr_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(close_lbl);

    lv_group_t* g = ui_focus_active_group();
    if (g) {
        lv_group_add_obj(g, close_btn);
        lv_group_focus_obj(close_btn);
    }
}

/* ── Node Name edit modal ────────────────────────────────────────────────── */

static lv_obj_t* s_name_label = NULL;
static lv_obj_t* s_name_edit_overlay = NULL;
static lv_obj_t* s_name_edit_ta = NULL;

static void name_edit_close(void) {
    if (!s_name_edit_overlay)
        return;
    ui_focus_pop_modal();
    /* Called from Cancel/Save INSIDE this overlay; defer the delete so the
     * clicked button survives its own event dispatch (see identity_qr_close). */
    lv_obj_delete_async(s_name_edit_overlay);
    s_name_edit_overlay = NULL;
    s_name_edit_ta = NULL;
}

static void name_edit_close_cb(lv_event_t* e) {
    (void)e;
    name_edit_close();
}

static void name_edit_save_cb(lv_event_t* e) {
    (void)e;
    if (!s_name_edit_ta)
        return;
    const char* new_name = lv_textarea_get_text(s_name_edit_ta);

    if (new_name && new_name[0] != '\0') {
        if (mesh_set_node_name_persist(new_name) == 0) {
            if (s_name_label)
                lv_label_set_text(s_name_label, new_name);
            ESP_LOGI(TAG, "Node name updated to: %s", new_name);
            ui_toast_show("Name saved");
        } else {
            ESP_LOGW(TAG, "Failed to persist node name");
            ui_toast_show("Save failed");
        }
        name_edit_close();
    } else {
        ui_toast_show("Name required"); /* keep the modal open */
    }
}

static void name_edit_cb(lv_event_t* e) {
    (void)e;
    if (s_name_edit_overlay)
        return;

    ui_focus_push_modal();

    lv_obj_t* scr = lv_screen_active();
    s_name_edit_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_name_edit_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_name_edit_overlay, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_name_edit_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_name_edit_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_name_edit_overlay, 0, 0);

    lv_obj_t* panel = lv_obj_create(s_name_edit_overlay);
    lv_obj_set_size(panel, 290, 132);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Set Node Name");
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    s_name_edit_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_name_edit_ta, 274, 36);
    lv_obj_align(s_name_edit_ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_max_length(s_name_edit_ta, 32);
    lv_textarea_set_one_line(s_name_edit_ta, true);
    const char* node_name = mesh_get_node_name();
    lv_textarea_set_text(s_name_edit_ta, node_name ? node_name : "");

    lv_obj_t* cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 128, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_add_event_cb(cancel_btn, name_edit_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t* save_btn = lv_btn_create(panel);
    lv_obj_set_size(save_btn, 128, 30);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(save_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_add_event_cb(save_btn, name_edit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);

    lv_group_t* g = ui_focus_active_group();
    if (g) {
        lv_group_add_obj(g, s_name_edit_ta);
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, save_btn);
        lv_group_focus_obj(s_name_edit_ta);
    }
}

/* ── Subpage ─────────────────────────────────────────────────────────────── */

void settings_identity_summary(char* buf, size_t n) {
    const char* node_name = mesh_get_node_name();
    snprintf(buf, n, "%s", (node_name && node_name[0]) ? node_name : "(not set)");
}

void settings_identity_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_t* cont = settings_subpage_begin(layout, "Identity");
    lv_group_t* g = lv_group_get_default();

    /* ── Node Name (editable) ── */
    lv_obj_t* name_row = settings_create_setting_row(cont, "Node Name");
    const char* node_name = mesh_get_node_name();
    ui_zone_track(&s_name_label, lv_label_create(name_row));
    lv_label_set_text(s_name_label, node_name ? node_name : "(not set)");
    lv_obj_set_style_text_color(s_name_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_name_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(name_row, name_edit_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, name_row);

    /* ── Node Identity QR share ── */
    lv_obj_t* identity_qr_row = settings_create_setting_row(cont, "Share Identity QR");
    lv_obj_t* identity_qr_hint = lv_label_create(identity_qr_row);
    lv_label_set_text(identity_qr_hint, "QR");
    lv_obj_set_style_text_color(identity_qr_hint, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(identity_qr_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(identity_qr_hint, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(identity_qr_row, identity_qr_open_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, identity_qr_row);

    /* ── Board info ── */
    lv_obj_t* board_row = settings_create_setting_row(cont, "Board");
    const bramble_board_config_t* board = board_get_config();
    lv_obj_t* board_val = lv_label_create(board_row);
    lv_label_set_text(board_val, board->name);
    lv_obj_set_style_text_color(board_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(board_val, &lv_font_montserrat_12, 0);
    lv_obj_align(board_val, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ── Separator ── */
    lv_obj_t* sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* ── Version ── */
    lv_obj_t* ver_row = settings_create_setting_row(cont, "Version");
    lv_obj_t* ver_val = lv_label_create(ver_row);
    lv_label_set_text(ver_val, esp_app_get_description()->version);
    lv_obj_set_style_text_color(ver_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(ver_val, &lv_font_montserrat_12, 0);
    lv_obj_align(ver_val, LV_ALIGN_RIGHT_MID, 0, 0);
}
