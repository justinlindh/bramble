#include "ui_pairing.h"
#include "sleep_manager.h"
#include "ui_focus.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"
#include <stdio.h>

/* Cross-task handoff: ui_pairing_passkey_cb runs on the NimBLE host task
 * (see ble_server.h) and this module's lv_timer polls on the LVGL task.
 * The request (passkey + show/hide) is packed into a single uint32 rather
 * than separate fields: a 32-bit aligned load/store is one instruction on
 * Xtensa, so it cannot tear, whereas writing passkey/show/valid as three
 * separate variables could let the timer observe a valid flag with a
 * stale or half-written passkey next to it. __atomic_store_n/exchange_n
 * add the acquire/release ordering the plain load/store instruction alone
 * doesn't guarantee, matching ui_graphics.c's s_pending_events handoff for
 * its own other-task-to-LVGL-task notifications. Single producer (the
 * NimBLE host task calls in are already serialized by the BLE stack),
 * single consumer (this module's timer): a newer request simply replaces
 * an unread older one, which matches "a fresh code per pairing attempt". */
#define PAIRING_VALID_BIT (1u << 31)
#define PAIRING_SHOW_BIT (1u << 30)
#define PAIRING_CODE_MASK 0x000FFFFFu /* passkey is 0..999999, fits in 20 bits */

static uint32_t s_pending;

static lv_obj_t* s_modal; /* NULL when hidden */
static lv_timer_t* s_timer;

void ui_pairing_passkey_cb(uint32_t passkey, bool show) {
    uint32_t req = PAIRING_VALID_BIT | (passkey & PAIRING_CODE_MASK);
    if (show) {
        req |= PAIRING_SHOW_BIT;
    }
    __atomic_store_n(&s_pending, req, __ATOMIC_RELEASE);
}

static void hide_modal(void) {
    if (s_modal != NULL) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
        /* Matches the ui_focus_push_modal in show_modal below: pop exactly
         * once per widget we actually put up, restoring keypad/trackball
         * input to the content zone. */
        ui_focus_pop_modal();
    }
}

static void show_modal(uint32_t code) {
    hide_modal();

    /* Pairing needs the user to read the screen; wake it if asleep. */
    sleep_manager_activity();

    /* Full-screen dim backdrop + centered panel, matching ui_confirm.c.
     * This modal has no buttons, so unlike ui_confirm it adds nothing to
     * the modal group ui_focus_push_modal creates: ui_zone_bind_indevs
     * rebinds every KEYPAD indev (trackball, keyboard, see
     * lv_port_trackball.c / lv_port_keyboard.c) onto that empty group, so
     * navigation and SELECT stop reaching the hidden screen behind this
     * overlay while it is up, without this module needing to build any
     * focusable widgets of its own. Confirmed safe on an empty group by
     * reading the vendored LVGL sources this build compiles against:
     * lv_group_create() (lv_group.c) sets obj_focus = NULL, and
     * lv_group_get_focused() returns NULL whenever obj_focus is NULL.
     * indev_keypad_proc() (lv_indev.c) is the single entry point every
     * keypad key event funnels through, and it does
     * `indev_obj_act = lv_group_get_focused(g); if (indev_obj_act == NULL)
     * return;` before any of the LV_KEY_NEXT/PREV/ENTER/ESC handling, so
     * every key on an empty group is dropped right there: no crash, no
     * event delivered anywhere, not even to whatever used to be focused
     * (that group is not even bound to any indev anymore). Dismissal is
     * still only ever driven by the show=false callback, never by user
     * input, so there is nothing for the user to do here regardless. The
     * overlay itself stays default-clickable (like ui_confirm's), so a
     * stray tap during pairing is absorbed here instead of reaching
     * whatever is behind it. */
    ui_focus_push_modal();

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, 260, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(panel, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "BLE pairing");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Enter this code on the connecting device:");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    /* code is 0..999999 (see ble_server.h); grouped as "NNN NNN" for
     * readability, same digit-grouping idea as scr_sas_verify.c's SAS
     * code. Digits and spaces are plain ASCII, so the glyph check has
     * nothing to flag regardless of font size. */
    char grouped[8];
    snprintf(grouped, sizeof(grouped), "%03u %03u", (unsigned)(code / 1000u),
              (unsigned)(code % 1000u));
    lv_obj_t* code_lbl = lv_label_create(panel);
    lv_label_set_text(code_lbl, grouped);
    /* montserrat_18 is the largest Montserrat size already compiled into
     * this build (scr_splash.c's title, scr_map.c's zoom buttons), and
     * this modal is the one place on screen when it is up, so the code
     * gets the biggest font available rather than scr_sas_verify.c's
     * inline 14pt SAS code. */
    lv_obj_set_style_text_font(code_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(code_lbl, BR_COLOR_PRIMARY, 0);
}

static void pairing_timer_cb(lv_timer_t* t) {
    (void)t;
    uint32_t req = __atomic_exchange_n(&s_pending, 0, __ATOMIC_ACQ_REL);
    if ((req & PAIRING_VALID_BIT) == 0) {
        return;
    }
    if (req & PAIRING_SHOW_BIT) {
        show_modal(req & PAIRING_CODE_MASK);
    } else {
        hide_modal();
    }
}

void ui_pairing_init(void) {
    s_timer = lv_timer_create(pairing_timer_cb, 100, NULL);
}
