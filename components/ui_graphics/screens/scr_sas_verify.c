#include "scr_sas_verify.h"
#include "scr_chat_messages.h"
#include "theme/bramble_theme.h"
#include "ui_confirm.h"
#include "ui_toast.h"
#include "sas_format.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdbool.h>

static const char* TAG = "scr_sas_verify";

/* mesh_ accessors live in main, not a component (same extern pattern as
 * scr_node_detail.c's mesh_send_location_packet). The UI never reaches into
 * identity_store directly. */
extern bool mesh_get_peer_verification(uint32_t addr, char sas_out[8], bool* verified,
                                       bool* key_changed);
extern bool mesh_set_peer_verified(uint32_t addr, bool verified);
extern const char* mesh_get_peer_name(uint32_t addr);

static bramble_layout_t* s_layout = NULL;
static uint32_t s_peer_addr = 0;

static void return_to_dm(void) {
    if (!s_layout || s_peer_addr == 0)
        return;
    scr_chat_messages_open_dm(s_layout, s_peer_addr);
}

static void back_click_cb(lv_event_t* e) {
    (void)e;
    return_to_dm();
}

static void on_verify_confirm(void* user_data) {
    uint32_t addr = (uint32_t)(uintptr_t)user_data;
    if (!mesh_set_peer_verified(addr, true)) {
        ESP_LOGW(TAG, "verify failed for %08lX (no pin)", (unsigned long)addr);
        ui_toast_show("Verify failed");
        return;
    }
    ui_toast_show("Verified");
    /* Return to the DM, whose header re-derives the glyph from the freshly
     * set verified bit. */
    return_to_dm();
}

static void codes_match_click_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_show("Codes match?", "Yes", on_verify_confirm, (void*)(uintptr_t)s_peer_addr);
}

static void add_action_btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, bool focus) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 180, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_radius(btn, BR_RADIUS, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, btn);
        if (focus)
            lv_group_focus_obj(btn);
    }
}

void scr_sas_verify_open(bramble_layout_t* layout, uint32_t peer_addr) {
    if (!layout || peer_addr == 0)
        return;

    s_layout = layout;
    s_peer_addr = peer_addr;

    char sas[8];
    bool verified = false;
    bool key_changed = false;
    if (!mesh_get_peer_verification(peer_addr, sas, &verified, &key_changed)) {
        ESP_LOGW(TAG, "no pin for %08lX; cannot show SAS", (unsigned long)peer_addr);
        ui_toast_show("No safety number yet");
        return;
    }

    char grouped[9];
    sas_format_grouped(sas, grouped);

    lv_obj_t* cont = layout_get_content(layout);
    lv_obj_clean(cont);

    lv_obj_t* card = lv_obj_create(cont);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, BR_PADDING, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char peer_line[32];
    const char* peer_name = mesh_get_peer_name(peer_addr);
    if (peer_name && peer_name[0]) {
        snprintf(peer_line, sizeof(peer_line), "%s", peer_name);
    } else {
        snprintf(peer_line, sizeof(peer_line), "%08lX", (unsigned long)peer_addr);
    }

    lv_obj_t* peer_lbl = lv_label_create(card);
    lv_label_set_text(peer_lbl, peer_line);
    lv_obj_set_style_text_font(peer_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(peer_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t* sas_lbl = lv_label_create(card);
    lv_label_set_text(sas_lbl, grouped);
    lv_obj_set_style_text_font(sas_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sas_lbl, BR_COLOR_PRIMARY, 0);

    lv_obj_t* hint_lbl = lv_label_create(card);
    lv_label_set_text(hint_lbl, "Read this aloud. It must match on both devices.");
    lv_obj_set_width(hint_lbl, lv_pct(100));
    lv_label_set_long_mode(hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);

    if (key_changed) {
        lv_obj_t* warn_lbl = lv_label_create(card);
        lv_label_set_text(warn_lbl, "Key changed since last verify");
        lv_obj_set_style_text_font(warn_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(warn_lbl, BR_COLOR_DANGER, 0);
    } else if (verified) {
        lv_obj_t* verified_lbl = lv_label_create(card);
        lv_label_set_text(verified_lbl, LV_SYMBOL_OK " already verified");
        lv_obj_set_style_text_font(verified_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(verified_lbl, BR_COLOR_PRIMARY, 0);
    }

    lv_obj_t* actions = lv_obj_create(card);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_row(actions, 6, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_action_btn(actions, "Codes Match", codes_match_click_cb, true);
    add_action_btn(actions, "Back", back_click_cb, false);
}
