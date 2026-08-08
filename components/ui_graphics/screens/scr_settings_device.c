#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_confirm.h"
#include "ui_toast.h"
#include "ui_zone.h"
#include "keyboard.h"
#include "audio.h"
#include "sleep_manager.h"
#include "bramble_tz.h"
#include "tz_store.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_set_dev";

/* ── Backlight ───────────────────────────────────────────────────────────── */

static void backlight_changed_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider); /* 0..100 */
    /* Persist backlight value to NVS and apply to hardware */
    keyboard_set_backlight_percent((uint8_t)val);
}

/* ── Volume ──────────────────────────────────────────────────────────────── */

static lv_obj_t* s_volume_slider = NULL;

static void volume_changed_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider); /* 0..100 */
    audio_set_volume((uint8_t)val);
}

static void volume_released_cb(lv_event_t* e) {
    (void)e;
    /* Play a short confirmation tone at the new volume so the user can hear
     * the result immediately. Skip if muted: that is expected silence. */
    if (!audio_get_muted()) {
        audio_play_beep(880, 80);
    }
}

/* ── Silent mode ─────────────────────────────────────────────────────────── */

static lv_obj_t* s_mute_sw = NULL;

static void mute_changed_cb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool muted = lv_obj_has_state(sw, LV_STATE_CHECKED);
    audio_set_muted(muted);

    /* Dim the volume slider when muted to hint it's inactive */
    if (s_volume_slider) {
        lv_obj_set_style_opa(s_volume_slider, muted ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}

/* ── Sleep mode ──────────────────────────────────────────────────────────── */

static lv_obj_t* s_sleep_timeout_slider = NULL;
static lv_obj_t* s_sleep_timeout_label = NULL;

static void sleep_enabled_cb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    sleep_manager_set_enabled(enabled);

    /* Dim the timeout slider and label when sleep is disabled */
    if (s_sleep_timeout_slider) {
        lv_obj_set_style_opa(s_sleep_timeout_slider, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    if (s_sleep_timeout_label) {
        lv_obj_set_style_opa(s_sleep_timeout_label, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void sleep_timeout_changed_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider); /* 10..300 seconds */
    sleep_manager_set_timeout((uint16_t)val);

    /* Update the value label */
    if (s_sleep_timeout_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ds", val);
        lv_label_set_text(s_sleep_timeout_label, buf);
    }
}

/* ── Time zone ───────────────────────────────────────────────────────────── */

/* The picker offers the named zones in the bramble_tz preset table. A spec set
 * over RPC that is not one of them still has to be representable, so the list
 * grows a trailing "Custom" entry in that case; selecting a named zone
 * replaces it. */
static lv_obj_t* s_tz_dropdown = NULL;
static bool s_tz_has_custom = false;

/* Index of the preset whose spec equals spec, or -1 when none does. */
static int tz_preset_index_of(const char* spec) {
    for (size_t i = 0; i < bramble_tz_preset_count(); i++) {
        if (strcmp(bramble_tz_preset(i)->spec, spec) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void tz_changed_cb(lv_event_t* e) {
    (void)e;
    if (!s_tz_dropdown) {
        return;
    }

    const uint16_t sel = lv_dropdown_get_selected(s_tz_dropdown);
    const size_t count = bramble_tz_preset_count();
    if (sel >= count) {
        /* The "Custom" row is the zone already stored: re-selecting it is a
         * no-op rather than a write. */
        return;
    }

    const bramble_tz_preset_t* preset = bramble_tz_preset(sel);
    if (tz_store_set(preset->spec) == 0) {
        ESP_LOGI(TAG, "Timezone set to %s (%s)", preset->label, preset->spec);
        ui_toast_show("Time zone saved");
    } else {
        ESP_LOGW(TAG, "Failed to persist timezone");
        ui_toast_show("Save failed");
    }
}

static void tz_build_row(lv_obj_t* cont, lv_group_t* g) {
    char spec[BRAMBLE_TZ_SPEC_MAX];
    tz_store_get(spec, sizeof(spec));
    const int preset_idx = tz_preset_index_of(spec);
    s_tz_has_custom = (preset_idx < 0);

    /* Presets, one label per line, plus the current custom spec when the
     * stored zone is not a named one. */
    char options[512];
    size_t used = 0;
    options[0] = '\0';
    for (size_t i = 0; i < bramble_tz_preset_count(); i++) {
        int n = snprintf(options + used, sizeof(options) - used, "%s%s", i == 0 ? "" : "\n",
                         bramble_tz_preset(i)->label);
        if (n < 0 || (size_t)n >= sizeof(options) - used) {
            break;
        }
        used += (size_t)n;
    }
    if (s_tz_has_custom) {
        snprintf(options + used, sizeof(options) - used, "\nCustom (%s)", spec);
    }

    lv_obj_t* tz_row = settings_create_setting_row(cont, LV_SYMBOL_BELL " Time Zone");
    lv_obj_set_size(tz_row, 304, 48);

    ui_zone_track(&s_tz_dropdown, lv_dropdown_create(tz_row));
    lv_dropdown_set_options(s_tz_dropdown, options);
    lv_obj_set_size(s_tz_dropdown, 170, 34);
    lv_obj_align(s_tz_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_set_style_bg_color(s_tz_dropdown, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_color(s_tz_dropdown, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_tz_dropdown, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_color(s_tz_dropdown, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_tz_dropdown, 1, 0);

    lv_obj_t* tz_list = lv_dropdown_get_list(s_tz_dropdown);
    lv_obj_set_style_bg_color(tz_list, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_color(tz_list, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(tz_list, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_color(tz_list, BR_COLOR_PRIMARY, 0);

    const int selected_idx = (preset_idx >= 0) ? preset_idx : (int)bramble_tz_preset_count();
    lv_dropdown_set_selected(s_tz_dropdown, (uint16_t)selected_idx);
    lv_obj_add_event_cb(s_tz_dropdown, tz_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) {
        lv_group_add_obj(g, s_tz_dropdown);
    }
}

/* ── Reboot ──────────────────────────────────────────────────────────────── */

static void do_reboot(void* user_data) {
    (void)user_data;
    ESP_LOGW(TAG, "Rebooting by user request...");
    esp_restart();
}

static void reboot_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_show("Reboot the device?", "Reboot", do_reboot, NULL);
}

/* ── Subpage ─────────────────────────────────────────────────────────────── */

void settings_device_summary(char* buf, size_t n) {
    uint8_t bl = keyboard_get_backlight_percent();
    bool sleep_on = sleep_manager_get_enabled();
    uint16_t timeout = sleep_manager_get_timeout();
    if (sleep_on) {
        snprintf(buf, n, "Backlight %s, sleep %us", bl > 0 ? "on" : "off", timeout);
    } else {
        snprintf(buf, n, "Backlight %s, sleep off", bl > 0 ? "on" : "off");
    }
}

void settings_device_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_t* cont = settings_subpage_begin(layout, "Device");
    lv_group_t* g = lv_group_get_default();

    /* ── Keyboard Backlight slider ── */
    uint8_t cur_backlight = keyboard_get_backlight_percent();

    lv_obj_t* bl_row = settings_create_setting_row(cont, "Backlight");
    lv_obj_set_size(bl_row, 304, 48);
    lv_obj_t* bl_slider = lv_slider_create(bl_row);
    lv_obj_set_size(bl_slider, 140, 10);
    lv_obj_align(bl_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(bl_slider, 0, 100);
    lv_slider_set_value(bl_slider, cur_backlight, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(bl_slider, backlight_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, bl_slider);

    /* ── Volume slider ── */
    uint8_t cur_vol = audio_get_volume();
    bool cur_muted = audio_get_muted();

    lv_obj_t* vol_row = settings_create_setting_row(cont, LV_SYMBOL_AUDIO " Volume");
    lv_obj_set_size(vol_row, 304, 48);
    ui_zone_track(&s_volume_slider, lv_slider_create(vol_row));
    lv_obj_set_size(s_volume_slider, 140, 10);
    lv_obj_align(s_volume_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_slider_set_value(s_volume_slider, cur_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_volume_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_volume_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_volume_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_volume_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume_slider, volume_released_cb, LV_EVENT_RELEASED, NULL);
    /* Dim the slider when already muted to hint it's inactive */
    if (cur_muted) {
        lv_obj_set_style_opa(s_volume_slider, LV_OPA_40, 0);
    }
    if (g)
        lv_group_add_obj(g, s_volume_slider);

    /* ── Silent mode toggle ── */
    lv_obj_t* mute_row = settings_create_setting_row(cont, LV_SYMBOL_MUTE " Silent");
    ui_zone_track(&s_mute_sw, lv_switch_create(mute_row));
    lv_obj_align(s_mute_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    /* Reflect saved mute state */
    if (cur_muted) {
        lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_mute_sw, mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_mute_sw);

    /* ── Sleep mode ── */
    bool cur_sleep_enabled = sleep_manager_get_enabled();
    uint16_t cur_sleep_timeout = sleep_manager_get_timeout();

    lv_obj_t* sleep_row = settings_create_setting_row(cont, LV_SYMBOL_EYE_CLOSE " Auto-Sleep");
    lv_obj_t* sleep_sw = lv_switch_create(sleep_row);
    lv_obj_align(sleep_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sleep_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sleep_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sleep_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    if (cur_sleep_enabled) {
        lv_obj_add_state(sleep_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sleep_sw, sleep_enabled_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, sleep_sw);

    /* Sleep timeout slider */
    lv_obj_t* sleep_timeout_row = settings_create_setting_row(cont, "Timeout");
    lv_obj_set_size(sleep_timeout_row, 304, 48);

    /* Value label showing "XXs" */
    ui_zone_track(&s_sleep_timeout_label, lv_label_create(sleep_timeout_row));
    char timeout_buf[16];
    snprintf(timeout_buf, sizeof(timeout_buf), "%us", cur_sleep_timeout);
    lv_label_set_text(s_sleep_timeout_label, timeout_buf);
    lv_obj_set_style_text_color(s_sleep_timeout_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_sleep_timeout_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_sleep_timeout_label, LV_ALIGN_RIGHT_MID, -150, 0);

    ui_zone_track(&s_sleep_timeout_slider, lv_slider_create(sleep_timeout_row));
    lv_obj_set_size(s_sleep_timeout_slider, 100, 10);
    lv_obj_align(s_sleep_timeout_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(s_sleep_timeout_slider, 10, 300);
    lv_slider_set_value(s_sleep_timeout_slider, cur_sleep_timeout, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sleep_timeout_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sleep_timeout_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sleep_timeout_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_sleep_timeout_slider, sleep_timeout_changed_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    /* Dim slider and label if sleep is disabled */
    if (!cur_sleep_enabled) {
        lv_obj_set_style_opa(s_sleep_timeout_slider, LV_OPA_40, 0);
        lv_obj_set_style_opa(s_sleep_timeout_label, LV_OPA_40, 0);
    }
    if (g)
        lv_group_add_obj(g, s_sleep_timeout_slider);

    /* ── Time zone ── */
    tz_build_row(cont, g);

    /* ── Reboot button ── */
    lv_obj_t* reboot_btn = lv_btn_create(cont);
    lv_obj_set_size(reboot_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(reboot_btn, BR_COLOR_DANGER, 0);
    lv_obj_set_style_radius(reboot_btn, BR_RADIUS, 0);
    lv_obj_t* reboot_lbl = lv_label_create(reboot_btn);
    lv_label_set_text(reboot_lbl, "Reboot Device");
    lv_obj_set_style_text_font(reboot_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(reboot_lbl);
    lv_obj_add_event_cb(reboot_btn, reboot_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, reboot_btn);
}
