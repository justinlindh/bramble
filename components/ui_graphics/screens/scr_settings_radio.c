#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_toast.h"
#include "ui_zone.h"
#include "radio.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>

static const char* TAG = "scr_set_radio";

static lv_obj_t* s_radio_tx_slider = NULL;
static lv_obj_t* s_radio_tx_label = NULL;
static lv_obj_t* s_radio_sf_dd = NULL;
static lv_obj_t* s_radio_bw_dd = NULL;
static lv_obj_t* s_radio_cr_dd = NULL;
static lv_obj_t* s_radio_freq_label = NULL;
static lv_obj_t* s_radio_apply_btn = NULL;
static bool s_radio_dirty = false;

static void radio_set_dirty(bool dirty);

static void radio_load_nvs_config(radio_config_t* cfg) {
    /* Start with current live config */
    radio_get_config(cfg);

    /* Overlay any NVS overrides */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_RADIO, NVS_READONLY, &nvs) != ESP_OK)
        return;

    uint32_t freq_khz = 0;
    if (nvs_get_u32(nvs, "freq_khz", &freq_khz) == ESP_OK)
        cfg->frequency_mhz = (float)freq_khz / 1000.0f;
    nvs_get_u8(nvs, "sf", &cfg->sf);
    nvs_get_u32(nvs, "bw_hz", &cfg->bw_hz);
    nvs_get_i8(nvs, "tx_power", &cfg->tx_power);
    nvs_get_u8(nvs, "cr", &cfg->coding_rate);
    nvs_close(nvs);
}

static void radio_save_and_apply(void) {
    if (!s_radio_tx_slider || !s_radio_sf_dd || !s_radio_bw_dd || !s_radio_cr_dd)
        return;

    radio_config_t cfg;
    radio_get_config(&cfg); /* base from live */

    cfg.tx_power = (int8_t)lv_slider_get_value(s_radio_tx_slider);

    uint16_t sf_sel = lv_dropdown_get_selected(s_radio_sf_dd);
    cfg.sf = (uint8_t)(sf_sel + 7); /* 0=SF7 .. 5=SF12 */

    uint16_t bw_sel = lv_dropdown_get_selected(s_radio_bw_dd);
    static const uint32_t bw_vals[] = {125000, 250000, 500000};
    cfg.bw_hz = bw_vals[bw_sel < 3 ? bw_sel : 0];

    uint16_t cr_sel = lv_dropdown_get_selected(s_radio_cr_dd);
    cfg.coding_rate = (uint8_t)(cr_sel + 5); /* 0=4/5 .. 3=4/8 */

    int rc = radio_reconfigure(&cfg);
    if (rc != 0) {
        ESP_LOGE(TAG, "radio_reconfigure failed");
        ui_toast_show("Radio apply failed");
        return;
    }

    /* Persist to NVS (mirrors rpc_methods.c) */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_RADIO, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, "freq_khz", (uint32_t)(cfg.frequency_mhz * 1000));
        nvs_set_u8(nvs, "sf", cfg.sf);
        nvs_set_u32(nvs, "bw_hz", cfg.bw_hz);
        nvs_set_i8(nvs, "tx_power", cfg.tx_power);
        nvs_set_u8(nvs, "cr", cfg.coding_rate);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Radio config saved: SF%u BW%lu TX%d CR4/%u", cfg.sf, (unsigned long)cfg.bw_hz,
             cfg.tx_power, cfg.coding_rate);
    ui_toast_show("Radio settings applied");
    radio_set_dirty(false); /* committed: disarm Apply until the next edit */
}

/* Apply is armed only by a real user edit. A page you only LOOKED at must be
 * impossible to commit: radio params are mesh-wide compatibility knobs, and an
 * accidental SELECT on an always-armed Apply is how a bench unit silently
 * dropped its TX power. */
static void radio_set_dirty(bool dirty) {
    s_radio_dirty = dirty;
    if (!s_radio_apply_btn)
        return;
    if (dirty) {
        lv_obj_remove_state(s_radio_apply_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_radio_apply_btn, LV_STATE_DISABLED);
    }
}

static void radio_mark_dirty_cb(lv_event_t* e) {
    (void)e;
    radio_set_dirty(true);
}

static void radio_tx_changed_cb(lv_event_t* e) {
    (void)e;
    if (s_radio_tx_label) {
        int val = lv_slider_get_value(s_radio_tx_slider);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d dBm", val);
        lv_label_set_text(s_radio_tx_label, buf);
    }
    radio_set_dirty(true);
}

static void radio_apply_cb(lv_event_t* e) {
    (void)e;
    if (!s_radio_dirty)
        return; /* disabled-state click or stale event: nothing to commit */
    radio_save_and_apply();
}

/* ── Subpage ─────────────────────────────────────────────────────────────── */

void settings_radio_summary(char* buf, size_t n) {
    radio_config_t cfg;
    radio_load_nvs_config(&cfg);
    snprintf(buf, n, "SF%u / %lukHz / %ddBm", cfg.sf, (unsigned long)(cfg.bw_hz / 1000),
             cfg.tx_power);
}

void settings_radio_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_t* cont = settings_subpage_begin(layout, "Radio");
    lv_group_t* g = lv_group_get_default();

    /* Matching-settings warning promoted to the top of the page. */
    lv_obj_t* warn = lv_label_create(cont);
    lv_label_set_text(warn, LV_SYMBOL_WARNING " All nodes must use matching\n"
                                              "radio settings to communicate.");
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(warn, BR_COLOR_WARNING, 0);

    radio_config_t cfg;
    radio_load_nvs_config(&cfg);

    /* Frequency (read-only display) */
    lv_obj_t* freq_row = settings_create_setting_row(cont, "Frequency");
    ui_zone_track(&s_radio_freq_label, lv_label_create(freq_row));
    char freq_buf[24];
    snprintf(freq_buf, sizeof(freq_buf), "%.1f MHz", (double)cfg.frequency_mhz);
    lv_label_set_text(s_radio_freq_label, freq_buf);
    lv_obj_set_style_text_color(s_radio_freq_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_radio_freq_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_radio_freq_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* TX Power slider */
    lv_obj_t* tx_row = settings_create_setting_row(cont, "TX Power");
    lv_obj_set_size(tx_row, 304, 48);

    ui_zone_track(&s_radio_tx_label, lv_label_create(tx_row));
    char tx_buf[16];
    snprintf(tx_buf, sizeof(tx_buf), "%d dBm", cfg.tx_power);
    lv_label_set_text(s_radio_tx_label, tx_buf);
    lv_obj_set_style_text_color(s_radio_tx_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_radio_tx_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_radio_tx_label, LV_ALIGN_RIGHT_MID, -110, 0);

    ui_zone_track(&s_radio_tx_slider, lv_slider_create(tx_row));
    lv_obj_set_size(s_radio_tx_slider, 100, 10);
    lv_obj_align(s_radio_tx_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    /* SX1262 tops out at +22 dBm and the fleet ships at 22. The old 2..20
     * range silently CLAMPED a stored 22 down to 20 at page build, so pressing
     * Apply with zero user edits downgraded TX power (this happened on the
     * bench: a stray trackball nudge plus a blind Apply persisted 19 dBm). */
    lv_slider_set_range(s_radio_tx_slider, 2, 22);
    lv_slider_set_value(s_radio_tx_slider, cfg.tx_power, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_radio_tx_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_radio_tx_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_radio_tx_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_radio_tx_slider, radio_tx_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_radio_tx_slider);

    /* Spreading Factor dropdown */
    lv_obj_t* sf_row = settings_create_setting_row(cont, "Spreading Factor");
    ui_zone_track(&s_radio_sf_dd, lv_dropdown_create(sf_row));
    lv_dropdown_set_options(s_radio_sf_dd, "SF7\nSF8\nSF9\nSF10\nSF11\nSF12");
    lv_obj_set_size(s_radio_sf_dd, 100, 34);
    lv_obj_align(s_radio_sf_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    if (cfg.sf >= 7 && cfg.sf <= 12)
        lv_dropdown_set_selected(s_radio_sf_dd, cfg.sf - 7);
    lv_obj_add_event_cb(s_radio_sf_dd, radio_mark_dirty_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_radio_sf_dd);

    /* Bandwidth dropdown */
    lv_obj_t* bw_row = settings_create_setting_row(cont, "Bandwidth");
    ui_zone_track(&s_radio_bw_dd, lv_dropdown_create(bw_row));
    lv_dropdown_set_options(s_radio_bw_dd, "125 kHz\n250 kHz\n500 kHz");
    lv_obj_set_size(s_radio_bw_dd, 110, 34);
    lv_obj_align(s_radio_bw_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    uint16_t bw_idx = 0;
    if (cfg.bw_hz == 250000)
        bw_idx = 1;
    else if (cfg.bw_hz == 500000)
        bw_idx = 2;
    lv_dropdown_set_selected(s_radio_bw_dd, bw_idx);
    lv_obj_add_event_cb(s_radio_bw_dd, radio_mark_dirty_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_radio_bw_dd);

    /* Coding Rate dropdown */
    lv_obj_t* cr_row = settings_create_setting_row(cont, "Coding Rate");
    ui_zone_track(&s_radio_cr_dd, lv_dropdown_create(cr_row));
    lv_dropdown_set_options(s_radio_cr_dd, "4/5\n4/6\n4/7\n4/8");
    lv_obj_set_size(s_radio_cr_dd, 100, 34);
    lv_obj_align(s_radio_cr_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    if (cfg.coding_rate >= 5 && cfg.coding_rate <= 8)
        lv_dropdown_set_selected(s_radio_cr_dd, cfg.coding_rate - 5);
    lv_obj_add_event_cb(s_radio_cr_dd, radio_mark_dirty_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_radio_cr_dd);

    /* Apply button: disarmed until an edit, muted while disarmed so the state
     * is visible from across the room. */
    lv_obj_t* apply_btn = lv_btn_create(cont);
    ui_zone_track(&s_radio_apply_btn, apply_btn);
    lv_obj_set_size(apply_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(apply_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_color(apply_btn, BR_COLOR_SURFACE_2, LV_STATE_DISABLED);
    lv_obj_set_style_radius(apply_btn, BR_RADIUS, 0);
    lv_obj_t* apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, LV_SYMBOL_OK " Apply Radio Settings");
    lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(apply_lbl, BR_COLOR_TEXT_SEC, LV_STATE_DISABLED);
    lv_obj_center(apply_lbl);
    lv_obj_add_event_cb(apply_btn, radio_apply_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, apply_btn);
    radio_set_dirty(false); /* fresh page: nothing to commit */
}
