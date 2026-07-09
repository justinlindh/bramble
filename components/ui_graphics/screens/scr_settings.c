#include <stdlib.h>
#include "airtime_budget.h"
#include "scr_settings.h"
#include "theme/bramble_theme.h"
#include "ui_confirm.h"
#include "esp_app_desc.h"
#include "ui_toast.h"
#include "display.h"
#include "keyboard.h"
#include "audio.h"
#include "sleep_manager.h"
#include "board_config.h"
#include "ui.h"
#include "location_settings_ui.h"
#include "location.h"
#include "routing.h"
#include "radio.h"
#include "channel_key.h"
#include "channel_msg.h"
#include "channel_storage.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "scr_settings";

/* Get/set node name from mesh — extern since it's in main */
extern int mesh_set_node_name_persist(const char* name);
extern const char* mesh_get_node_name(void);
extern int mesh_get_identity(uint32_t* addr_out, uint8_t pubkey_out[32]);

/* Mesh state snapshot — mirrored from mesh_task.h to avoid include cycle */
typedef struct {
    neighbor_table_t neighbors;
    uint32_t beacon_tx_count;
    uint32_t beacon_rx_count;
    uint32_t packets_tx;
    uint32_t packets_rx;
    bool radio_ok;
    int16_t last_rx_rssi;
    int8_t last_rx_snr;
    airtime_budget_t airtime;
} settings_mesh_state_t;
extern void mesh_get_state(settings_mesh_state_t* out);

/* ── Backlight ───────────────────────────────────────────────────────── */

static void backlight_changed_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider); /* 0..100 */
    /* Persist backlight value to NVS and apply to hardware */
    keyboard_set_backlight_percent((uint8_t)val);
}

/* ── Volume ──────────────────────────────────────────────────────────── */

static lv_obj_t* s_volume_slider = NULL;

static void volume_changed_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider); /* 0..100 */
    audio_set_volume((uint8_t)val);
}

static void volume_released_cb(lv_event_t* e) {
    (void)e;
    /* Play a short confirmation tone at the new volume so the user can hear
     * the result immediately.  Skip if muted — that's expected silence. */
    if (!audio_get_muted()) {
        audio_play_beep(880, 80);
    }
}

/* ── Silent mode ─────────────────────────────────────────────────────── */

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

/* ── Sleep mode ─────────────────────────────────────────────────────── */

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

/* ── Location sharing controls ───────────────────────────────────────── */

static lv_obj_t* s_loc_share_sw = NULL;
static lv_obj_t* s_loc_tier_dd = NULL;
static lv_obj_t* s_loc_interval_dd = NULL;
static lv_obj_t* s_loc_source_lbl = NULL;
static lv_obj_t* s_loc_last_share_lbl = NULL;
static location_ui_state_t s_loc_state;

/* ── Location peer targets ───────────────────────────────────────────── */

/* Per-peer contact record stored in NVS blob "ct_data" under "bramble_loc" */
typedef struct {
    uint32_t addr;
    uint8_t tier; /* LOCATION_UI_TIER_* — 0xFF = use global */
    uint8_t enabled;
} loc_contact_nvs_t;

#define LOC_CONTACT_UI_MAX LOCATION_MAX_CONTACTS /* 16 */

static loc_contact_nvs_t s_loc_contacts[LOC_CONTACT_UI_MAX];
static int s_loc_contact_count = 0;

static int location_contacts_find(uint32_t addr) {
    for (int i = 0; i < s_loc_contact_count; i++) {
        if (s_loc_contacts[i].addr == addr)
            return i;
    }
    return -1;
}

static void location_contacts_load(void) {
    s_loc_contact_count = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK)
        return;
    size_t len = sizeof(s_loc_contacts);
    nvs_get_blob(nvs, "ct_data", s_loc_contacts, &len);
    if (len > 0 && (len % sizeof(loc_contact_nvs_t)) == 0) {
        s_loc_contact_count = (int)(len / sizeof(loc_contact_nvs_t));
        if (s_loc_contact_count > LOC_CONTACT_UI_MAX)
            s_loc_contact_count = LOC_CONTACT_UI_MAX;
    }
    nvs_close(nvs);
}

static void location_contacts_save(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK)
        return;
    nvs_set_blob(nvs, "ct_data", s_loc_contacts,
                 (size_t)(s_loc_contact_count) * sizeof(loc_contact_nvs_t));
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Toggle callback — user_data encodes peer addr as uintptr_t */
static void location_contact_toggle_cb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool want_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint32_t addr = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    int idx = location_contacts_find(addr);
    if (want_enabled) {
        if (idx < 0) {
            /* Add new entry if space available */
            if (s_loc_contact_count < LOC_CONTACT_UI_MAX) {
                s_loc_contacts[s_loc_contact_count].addr = addr;
                s_loc_contacts[s_loc_contact_count].tier = 0xFF; /* use global */
                s_loc_contacts[s_loc_contact_count].enabled = 1;
                s_loc_contact_count++;
            }
        } else {
            s_loc_contacts[idx].enabled = 1;
        }
    } else {
        if (idx >= 0) {
            /* Remove by shifting */
            for (int i = idx; i < s_loc_contact_count - 1; i++) {
                s_loc_contacts[i] = s_loc_contacts[i + 1];
            }
            s_loc_contact_count--;
        }
    }
    location_contacts_save();
    ESP_LOGI(TAG, "Peer target %08lX %s", (unsigned long)addr, want_enabled ? "added" : "removed");
}

static void location_ui_load_state(location_ui_state_t* st) {
    if (!st)
        return;

    st->sharing_enabled = false;
    st->tier = LOCATION_UI_TIER_COARSE;
    st->interval_s = LOCATION_UI_INTERVAL_5_MIN;
    st->source = LOCATION_UI_SOURCE_HYBRID;
    st->last_share_epoch_s = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK)
        return;

    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK)
        st->sharing_enabled = (enabled != 0);

    uint16_t interval_s = 0;
    if (nvs_get_u16(nvs, "interval_s", &interval_s) == ESP_OK) {
        if (interval_s == LOCATION_UI_INTERVAL_1_MIN || interval_s == LOCATION_UI_INTERVAL_5_MIN ||
            interval_s == LOCATION_UI_INTERVAL_15_MIN ||
            interval_s == LOCATION_UI_INTERVAL_60_MIN) {
            st->interval_s = interval_s;
        }
    }

    char tier[16] = {0};
    size_t tier_len = sizeof(tier);
    if (nvs_get_str(nvs, "def_tier", tier, &tier_len) == ESP_OK) {
        if (strcmp(tier, "exact") == 0 || strcmp(tier, "full") == 0)
            st->tier = LOCATION_UI_TIER_FULL;
        else if (strcmp(tier, "presence") == 0)
            st->tier = LOCATION_UI_TIER_PRESENCE;
        else
            st->tier = LOCATION_UI_TIER_COARSE;
    }

    char source[16] = {0};
    size_t source_len = sizeof(source);
    if (nvs_get_str(nvs, "source", source, &source_len) == ESP_OK) {
        if (strcmp(source, "gps") == 0)
            st->source = LOCATION_UI_SOURCE_GPS;
        else if (strcmp(source, "manual") == 0)
            st->source = LOCATION_UI_SOURCE_MANUAL;
        else
            st->source = LOCATION_UI_SOURCE_HYBRID;
    }

    uint32_t last_share = 0;
    if (nvs_get_u32(nvs, "last_share_s", &last_share) == ESP_OK)
        st->last_share_epoch_s = last_share;

    nvs_close(nvs);
}

static void location_ui_save_state(const location_ui_state_t* st) {
    if (!st)
        return;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK)
        return;

    nvs_set_u8(nvs, "enabled", st->sharing_enabled ? 1 : 0);
    nvs_set_u16(nvs, "interval_s", st->interval_s);

    const char* tier = "coarse";
    if (st->tier == LOCATION_UI_TIER_FULL)
        tier = "exact";
    else if (st->tier == LOCATION_UI_TIER_PRESENCE)
        tier = "presence";
    nvs_set_str(nvs, "def_tier", tier);

    const char* source = "hybrid";
    if (st->source == LOCATION_UI_SOURCE_GPS)
        source = "gps";
    else if (st->source == LOCATION_UI_SOURCE_MANUAL)
        source = "manual";
    nvs_set_str(nvs, "source", source);

    nvs_commit(nvs);
    nvs_close(nvs);
}

static uint16_t location_interval_from_dropdown(uint16_t selected) {
    switch (selected) {
    case 0:
        return LOCATION_UI_INTERVAL_1_MIN;
    case 1:
        return LOCATION_UI_INTERVAL_5_MIN;
    case 2:
        return LOCATION_UI_INTERVAL_15_MIN;
    case 3:
        return LOCATION_UI_INTERVAL_60_MIN;
    default:
        return LOCATION_UI_INTERVAL_5_MIN;
    }
}

static uint16_t location_interval_to_dropdown(uint16_t interval_s) {
    switch (interval_s) {
    case LOCATION_UI_INTERVAL_1_MIN:
        return 0;
    case LOCATION_UI_INTERVAL_5_MIN:
        return 1;
    case LOCATION_UI_INTERVAL_15_MIN:
        return 2;
    case LOCATION_UI_INTERVAL_60_MIN:
        return 3;
    default:
        return 1;
    }
}

static void location_refresh_status_labels(void) {
    if (s_loc_source_lbl) {
        lv_label_set_text_fmt(s_loc_source_lbl, "%s", location_ui_source_label(s_loc_state.source));
    }
    if (s_loc_last_share_lbl) {
        char buf[24];
        uint32_t now_s = (uint32_t)time(NULL);
        location_ui_format_last_share(buf, sizeof(buf), s_loc_state.last_share_epoch_s, now_s);
        lv_label_set_text(s_loc_last_share_lbl, buf);
    }
}

static void location_share_changed_cb(lv_event_t* e) {
    bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_SET_SHARING, enabled ? 1 : 0);
    location_ui_save_state(&s_loc_state);
}

static void location_tier_changed_cb(lv_event_t* e) {
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    location_ui_tier_t tier = LOCATION_UI_TIER_COARSE;
    if (sel == 0)
        tier = LOCATION_UI_TIER_COARSE;
    else if (sel == 1)
        tier = LOCATION_UI_TIER_FULL;
    else if (sel == 2)
        tier = LOCATION_UI_TIER_PRESENCE;
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_SET_TIER, tier);
    location_ui_save_state(&s_loc_state);
}

static void location_interval_changed_cb(lv_event_t* e) {
    uint16_t interval_s =
        location_interval_from_dropdown(lv_dropdown_get_selected(lv_event_get_target(e)));
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_SET_INTERVAL, interval_s);
    location_ui_save_state(&s_loc_state);
}

static void do_panic_off(void* user_data) {
    (void)user_data;
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_PANIC_OFF, 0);
    location_ui_save_state(&s_loc_state);
    if (s_loc_share_sw)
        lv_obj_clear_state(s_loc_share_sw, LV_STATE_CHECKED);
    ui_toast_show("Location sharing off");
}

static void location_panic_off_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_show("Turn off all location sharing?", "Panic Off", do_panic_off, NULL);
}

/* ── Peer targets section builder ───────────────────────────────────── */

static void build_loc_peer_targets_section(lv_obj_t* cont, lv_group_t* g) {
    /* Section separator */
    lv_obj_t* sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    lv_obj_t* section_lbl = lv_label_create(cont);
    lv_label_set_text(section_lbl, LV_SYMBOL_CALL " Peer Targets");
    lv_obj_set_style_text_font(section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t* hint = lv_label_create(cont);
    lv_label_set_text(hint, "Send location to specific peers.\n"
                            "Uses global tier unless overridden.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);

    /* Get current neighbor snapshot (heap-allocated — too large for 8KB task stack) */
    settings_mesh_state_t* mesh = malloc(sizeof(settings_mesh_state_t));
    if (!mesh) {
        ESP_LOGE("settings", "Failed to allocate mesh state for location peers");
        return;
    }
    mesh_get_state(mesh);
    int n_count = neighbor_count(&mesh->neighbors);

    if (n_count == 0) {
        lv_obj_t* no_peers = lv_label_create(cont);
        lv_label_set_text(no_peers, "(no peers visible)");
        lv_obj_set_style_text_font(no_peers, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(no_peers, BR_COLOR_TEXT_SEC, 0);
        free(mesh);
        return;
    }

    /* Render one toggle row per neighbor entry */
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const neighbor_entry_t* nb = &mesh->neighbors.entries[i];
        if (nb->addr == 0)
            continue; /* empty slot */

        /* Row container */
        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Label: name or hex addr */
        lv_obj_t* lbl = lv_label_create(row);
        if (nb->name[0]) {
            lv_label_set_text(lbl, nb->name);
        } else {
            char hex[12];
            snprintf(hex, sizeof(hex), "%08lX", (unsigned long)nb->addr);
            lv_label_set_text(lbl, hex);
        }
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_set_width(lbl, 200);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Toggle switch */
        lv_obj_t* sw = lv_switch_create(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sw, BR_COLOR_TEXT, LV_PART_KNOB);

        /* Pre-check if this peer is already a target */
        if (location_contacts_find(nb->addr) >= 0) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }

        lv_obj_add_event_cb(sw, location_contact_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void*)(uintptr_t)nb->addr);
        if (g)
            lv_group_add_obj(g, sw);
    }

    /* Show persisted offline targets (not currently visible as neighbors) */
    bool shown_offline_hdr = false;
    for (int ci = 0; ci < s_loc_contact_count; ci++) {
        const loc_contact_nvs_t* ct = &s_loc_contacts[ci];
        /* Check if already shown as a neighbor */
        bool is_neighbor = false;
        for (int ni = 0; ni < MAX_NEIGHBORS; ni++) {
            if (mesh->neighbors.entries[ni].addr == ct->addr) {
                is_neighbor = true;
                break;
            }
        }
        if (is_neighbor)
            continue;

        if (!shown_offline_hdr) {
            lv_obj_t* off_lbl = lv_label_create(cont);
            lv_label_set_text(off_lbl, "Saved (offline):");
            lv_obj_set_style_text_font(off_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(off_lbl, BR_COLOR_TEXT_SEC, 0);
            shown_offline_hdr = true;
        }

        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char hex[12];
        snprintf(hex, sizeof(hex), "%08lX", (unsigned long)ct->addr);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, hex);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_width(lbl, 200);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* sw = lv_switch_create(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sw, BR_COLOR_TEXT, LV_PART_KNOB);
        if (ct->enabled)
            lv_obj_add_state(sw, LV_STATE_CHECKED);

        lv_obj_add_event_cb(sw, location_contact_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void*)(uintptr_t)ct->addr);
        if (g)
            lv_group_add_obj(g, sw);
    }

    free(mesh);
}

/* ── Connectivity mode toggle ────────────────────────────────────────── */

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

/* ── Reboot ──────────────────────────────────────────────────────────── */

static void do_reboot(void* user_data) {
    (void)user_data;
    ESP_LOGW(TAG, "Rebooting by user request...");
    esp_restart();
}

static void reboot_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_show("Reboot the device?", "Reboot", do_reboot, NULL);
}

/* ── Helper: labeled setting row ─────────────────────────────────────── */

static lv_obj_t* create_setting_row(lv_obj_t* parent, const char* label) {
    lv_obj_t* row = lv_obj_create(parent);
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

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    return row;
}

/* ── Identity QR share modal ─────────────────────────────────────────── */

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
    lv_obj_delete(s_identity_qr_overlay);
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

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, close_btn);
        lv_group_focus_obj(close_btn);
    }
}

/* ── Node Name edit modal ────────────────────────────────────────────── */

static lv_obj_t* s_name_label = NULL;
static lv_obj_t* s_name_edit_overlay = NULL;
static lv_obj_t* s_name_edit_ta = NULL;

static void name_edit_close(void) {
    if (!s_name_edit_overlay)
        return;
    lv_obj_delete(s_name_edit_overlay);
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

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_name_edit_ta);
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, save_btn);
        lv_group_focus_obj(s_name_edit_ta);
    }
}

/* ── Radio Configuration ─────────────────────────────────────────────── */

static lv_obj_t* s_radio_tx_slider = NULL;
static lv_obj_t* s_radio_tx_label = NULL;
static lv_obj_t* s_radio_sf_dd = NULL;
static lv_obj_t* s_radio_bw_dd = NULL;
static lv_obj_t* s_radio_cr_dd = NULL;
static lv_obj_t* s_radio_freq_label = NULL;

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
}

static void radio_tx_changed_cb(lv_event_t* e) {
    (void)e;
    if (s_radio_tx_label) {
        int val = lv_slider_get_value(s_radio_tx_slider);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d dBm", val);
        lv_label_set_text(s_radio_tx_label, buf);
    }
}

static void radio_apply_cb(lv_event_t* e) {
    (void)e;
    radio_save_and_apply();
}

static void build_radio_config_section(lv_obj_t* cont, lv_group_t* g) {
    /* Separator */
    lv_obj_t* sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    lv_obj_t* section_lbl = lv_label_create(cont);
    lv_label_set_text(section_lbl, LV_SYMBOL_SETTINGS " Radio Config");
    lv_obj_set_style_text_font(section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section_lbl, BR_COLOR_TEXT, 0);

    radio_config_t cfg;
    radio_load_nvs_config(&cfg);

    /* Frequency (read-only display) */
    lv_obj_t* freq_row = create_setting_row(cont, "Frequency");
    s_radio_freq_label = lv_label_create(freq_row);
    char freq_buf[24];
    snprintf(freq_buf, sizeof(freq_buf), "%.1f MHz", (double)cfg.frequency_mhz);
    lv_label_set_text(s_radio_freq_label, freq_buf);
    lv_obj_set_style_text_color(s_radio_freq_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_radio_freq_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_radio_freq_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* TX Power slider */
    lv_obj_t* tx_row = create_setting_row(cont, "TX Power");
    lv_obj_set_size(tx_row, 304, 48);

    s_radio_tx_label = lv_label_create(tx_row);
    char tx_buf[16];
    snprintf(tx_buf, sizeof(tx_buf), "%d dBm", cfg.tx_power);
    lv_label_set_text(s_radio_tx_label, tx_buf);
    lv_obj_set_style_text_color(s_radio_tx_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_radio_tx_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_radio_tx_label, LV_ALIGN_RIGHT_MID, -110, 0);

    s_radio_tx_slider = lv_slider_create(tx_row);
    lv_obj_set_size(s_radio_tx_slider, 100, 10);
    lv_obj_align(s_radio_tx_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(s_radio_tx_slider, 2, 20);
    lv_slider_set_value(s_radio_tx_slider, cfg.tx_power, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_radio_tx_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_radio_tx_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_radio_tx_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_radio_tx_slider, radio_tx_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_radio_tx_slider);

    /* Spreading Factor dropdown */
    lv_obj_t* sf_row = create_setting_row(cont, "Spreading Factor");
    s_radio_sf_dd = lv_dropdown_create(sf_row);
    lv_dropdown_set_options(s_radio_sf_dd, "SF7\nSF8\nSF9\nSF10\nSF11\nSF12");
    lv_obj_set_size(s_radio_sf_dd, 100, 34);
    lv_obj_align(s_radio_sf_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    if (cfg.sf >= 7 && cfg.sf <= 12)
        lv_dropdown_set_selected(s_radio_sf_dd, cfg.sf - 7);
    if (g)
        lv_group_add_obj(g, s_radio_sf_dd);

    /* Bandwidth dropdown */
    lv_obj_t* bw_row = create_setting_row(cont, "Bandwidth");
    s_radio_bw_dd = lv_dropdown_create(bw_row);
    lv_dropdown_set_options(s_radio_bw_dd, "125 kHz\n250 kHz\n500 kHz");
    lv_obj_set_size(s_radio_bw_dd, 110, 34);
    lv_obj_align(s_radio_bw_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    uint16_t bw_idx = 0;
    if (cfg.bw_hz == 250000)
        bw_idx = 1;
    else if (cfg.bw_hz == 500000)
        bw_idx = 2;
    lv_dropdown_set_selected(s_radio_bw_dd, bw_idx);
    if (g)
        lv_group_add_obj(g, s_radio_bw_dd);

    /* Coding Rate dropdown */
    lv_obj_t* cr_row = create_setting_row(cont, "Coding Rate");
    s_radio_cr_dd = lv_dropdown_create(cr_row);
    lv_dropdown_set_options(s_radio_cr_dd, "4/5\n4/6\n4/7\n4/8");
    lv_obj_set_size(s_radio_cr_dd, 100, 34);
    lv_obj_align(s_radio_cr_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    if (cfg.coding_rate >= 5 && cfg.coding_rate <= 8)
        lv_dropdown_set_selected(s_radio_cr_dd, cfg.coding_rate - 5);
    if (g)
        lv_group_add_obj(g, s_radio_cr_dd);

    /* Apply button */
    lv_obj_t* apply_btn = lv_btn_create(cont);
    lv_obj_set_size(apply_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(apply_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(apply_btn, BR_RADIUS, 0);
    lv_obj_t* apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, LV_SYMBOL_OK " Apply Radio Settings");
    lv_obj_set_style_text_font(apply_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(apply_lbl);
    lv_obj_add_event_cb(apply_btn, radio_apply_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, apply_btn);

    lv_obj_t* hint = lv_label_create(cont);
    lv_label_set_text(hint, "All nodes must use matching radio\n"
                            "settings to communicate.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);
}

/* ── Channel Manager ─────────────────────────────────────────────────── */

extern int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len);
extern int mesh_remove_channel(int index);
extern int mesh_get_channel_count(void);
extern const char* mesh_get_channel_name(int index);
extern int mesh_get_channel_security(int index, bool* has_psk, uint16_t* epoch);
extern int mesh_set_default_channel(int index);
extern int mesh_get_channel_info(int* default_idx);

/* Forward declare so callbacks can rebuild the section */
static void build_channel_manager_section(lv_obj_t* cont, lv_group_t* g);

static lv_obj_t* s_channel_section_cont = NULL;
static lv_group_t* s_channel_group = NULL;

/* Add channel modal */
static lv_obj_t* s_ch_add_overlay = NULL;
static lv_obj_t* s_ch_name_ta = NULL;
static lv_obj_t* s_ch_psk_ta = NULL;

static void channel_add_close(void) {
    if (s_ch_add_overlay) {
        lv_obj_delete(s_ch_add_overlay);
        s_ch_add_overlay = NULL;
        s_ch_name_ta = NULL;
        s_ch_psk_ta = NULL;
    }
}

static void channel_refresh_list(void);

static void channel_add_save_cb(lv_event_t* e) {
    (void)e;
    if (!s_ch_name_ta)
        return;
    const char* name = lv_textarea_get_text(s_ch_name_ta);
    const char* psk = s_ch_psk_ta ? lv_textarea_get_text(s_ch_psk_ta) : NULL;

    if (!name || name[0] == '\0') {
        ui_toast_show("Channel name required");
        return; /* keep the modal open for correction */
    }

    size_t psk_len = (psk && psk[0]) ? strlen(psk) : 0;
    int idx = mesh_add_channel(name, psk_len > 0 ? (const uint8_t*)psk : NULL, psk_len);
    if (idx >= 0) {
        ESP_LOGI(TAG, "Channel '%s' added at index %d", name, idx);
        ui_toast_show("Channel created");
    } else {
        ESP_LOGE(TAG, "Failed to add channel '%s'", name);
        ui_toast_show("Failed to add channel");
    }
    channel_add_close();
    channel_refresh_list();
}

static void channel_add_cancel_cb(lv_event_t* e) {
    (void)e;
    channel_add_close();
}

static void channel_add_open_cb(lv_event_t* e) {
    (void)e;
    if (s_ch_add_overlay)
        return;

    lv_obj_t* scr = lv_screen_active();
    s_ch_add_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_ch_add_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ch_add_overlay, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ch_add_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ch_add_overlay, 0, 0);

    lv_obj_t* panel = lv_obj_create(s_ch_add_overlay);
    lv_obj_set_size(panel, 290, 170);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Add Channel");
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    s_ch_name_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_ch_name_ta, 270, 36);
    lv_textarea_set_max_length(s_ch_name_ta, 19);
    lv_textarea_set_one_line(s_ch_name_ta, true);
    lv_textarea_set_placeholder_text(s_ch_name_ta, "Channel name");
    lv_obj_set_style_bg_color(s_ch_name_ta, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_font(s_ch_name_ta, &lv_font_montserrat_12, 0);

    s_ch_psk_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_ch_psk_ta, 270, 36);
    lv_textarea_set_max_length(s_ch_psk_ta, 31);
    lv_textarea_set_one_line(s_ch_psk_ta, true);
    lv_textarea_set_placeholder_text(s_ch_psk_ta, "PSK (optional)");
    lv_obj_set_style_bg_color(s_ch_psk_ta, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_font(s_ch_psk_ta, &lv_font_montserrat_12, 0);

    lv_obj_t* btn_row = lv_obj_create(panel);
    lv_obj_set_size(btn_row, 270, 36);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 120, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_add_event_cb(cancel_btn, channel_add_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t* save_btn = lv_btn_create(btn_row);
    lv_obj_set_size(save_btn, 120, 30);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_add_event_cb(save_btn, channel_add_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Add");
    lv_obj_center(save_lbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_ch_name_ta);
        lv_group_add_obj(g, s_ch_psk_ta);
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, save_btn);
        lv_group_focus_obj(s_ch_name_ta);
    }
}

static void do_remove_channel(void* user_data) {
    int index = (int)(intptr_t)user_data;
    int rc = mesh_remove_channel(index);
    if (rc == 0) {
        ESP_LOGI(TAG, "Channel %d removed", index);
        ui_toast_show("Channel removed");
    } else {
        ESP_LOGE(TAG, "Failed to remove channel %d", index);
        ui_toast_show("Remove failed");
    }
    channel_refresh_list();
}

static void channel_remove_cb(lv_event_t* e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    ui_confirm_show("Remove this channel?", "Remove", do_remove_channel, (void*)(intptr_t)index);
}

static void channel_set_default_cb(lv_event_t* e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    mesh_set_default_channel(index);
    ESP_LOGI(TAG, "Default channel set to %d", index);
    ui_toast_show("Default channel set");
    channel_refresh_list();
}

static void channel_refresh_list(void) {
    if (!s_channel_section_cont)
        return;
    /* Delete all children of the channel section and rebuild */
    lv_obj_clean(s_channel_section_cont);
    build_channel_manager_section(s_channel_section_cont, s_channel_group);
}

static void build_channel_manager_section(lv_obj_t* cont, lv_group_t* g) {
    s_channel_section_cont = cont;
    s_channel_group = g;

    lv_obj_t* section_lbl = lv_label_create(cont);
    lv_label_set_text(section_lbl, LV_SYMBOL_LIST " Channels");
    lv_obj_set_style_text_font(section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section_lbl, BR_COLOR_TEXT, 0);

    int num_channels = mesh_get_channel_count();
    int default_idx = -1;
    mesh_get_channel_info(&default_idx);

    if (num_channels == 0) {
        lv_obj_t* empty = lv_label_create(cont);
        lv_label_set_text(empty, "(no channels)");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
    }

    for (int i = 0; i < num_channels; i++) {
        const char* name = mesh_get_channel_name(i);
        bool has_psk = false;
        uint16_t epoch = 0;
        mesh_get_channel_security(i, &has_psk, &epoch);
        bool is_default = (i == default_idx);

        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Channel index + name */
        lv_obj_t* lbl = lv_label_create(row);
        char ch_text[48];
        snprintf(ch_text, sizeof(ch_text), "#%d %s%s%s", i, name ? name : "?",
                 has_psk ? " " LV_SYMBOL_EYE_CLOSE : "", is_default ? " *" : "");
        lv_label_set_text(lbl, ch_text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, is_default ? BR_COLOR_PRIMARY : BR_COLOR_TEXT, 0);
        lv_obj_set_width(lbl, 160);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Set default button (if not already default) */
        if (!is_default) {
            lv_obj_t* def_btn = lv_btn_create(row);
            lv_obj_set_size(def_btn, 50, 24);
            lv_obj_align(def_btn, LV_ALIGN_RIGHT_MID, -40, 0);
            lv_obj_set_style_bg_color(def_btn, BR_COLOR_SURFACE_2, 0);
            lv_obj_set_style_radius(def_btn, 4, 0);
            lv_obj_t* def_lbl = lv_label_create(def_btn);
            lv_label_set_text(def_lbl, "Def");
            lv_obj_set_style_text_font(def_lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(def_lbl);
            lv_obj_add_event_cb(def_btn, channel_set_default_cb, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);
            if (g)
                lv_group_add_obj(g, def_btn);
        }

        /* Remove button */
        if (!is_default) {
            lv_obj_t* rm_btn = lv_btn_create(row);
            lv_obj_set_size(rm_btn, 30, 24);
            lv_obj_align(rm_btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(rm_btn, BR_COLOR_DANGER, 0);
            lv_obj_set_style_radius(rm_btn, 4, 0);
            lv_obj_t* rm_lbl = lv_label_create(rm_btn);
            lv_label_set_text(rm_lbl, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_font(rm_lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(rm_lbl);
            lv_obj_add_event_cb(rm_btn, channel_remove_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            if (g)
                lv_group_add_obj(g, rm_btn);
        }
    }

    /* Add channel button */
    lv_obj_t* add_btn = lv_btn_create(cont);
    lv_obj_set_size(add_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(add_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(add_btn, BR_RADIUS, 0);
    lv_obj_t* add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Add Channel");
    lv_obj_set_style_text_font(add_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(add_lbl);
    lv_obj_add_event_cb(add_btn, channel_add_open_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, add_btn);
}

/* ── Peer Manager ────────────────────────────────────────────────────── */

static void build_peer_manager_section(lv_obj_t* cont, lv_group_t* g) {
    /* Separator */
    lv_obj_t* sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    lv_obj_t* section_lbl = lv_label_create(cont);
    lv_label_set_text(section_lbl, LV_SYMBOL_CALL " Known Peers");
    lv_obj_set_style_text_font(section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section_lbl, BR_COLOR_TEXT, 0);

    settings_mesh_state_t* mesh = malloc(sizeof(settings_mesh_state_t));
    if (!mesh) {
        ESP_LOGE("settings", "Failed to allocate mesh state for peer manager");
        return;
    }
    mesh_get_state(mesh);
    int n_count = neighbor_count(&mesh->neighbors);

    if (n_count == 0) {
        lv_obj_t* empty = lv_label_create(cont);
        lv_label_set_text(empty, "(no peers discovered)");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
        free(mesh);
        return;
    }

    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const neighbor_entry_t* nb = &mesh->neighbors.entries[i];
        if (nb->addr == 0)
            continue;

        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Name or hex addr */
        lv_obj_t* lbl = lv_label_create(row);
        if (nb->name[0]) {
            lv_label_set_text(lbl, nb->name);
        } else {
            char hex[12];
            snprintf(hex, sizeof(hex), "%08lX", (unsigned long)nb->addr);
            lv_label_set_text(lbl, hex);
        }
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_set_width(lbl, 160);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* RSSI indicator */
        lv_obj_t* rssi_lbl = lv_label_create(row);
        char rssi_buf[24];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", nb->rssi);
        lv_label_set_text(rssi_lbl, rssi_buf);
        lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rssi_lbl, BR_COLOR_TEXT_SEC, 0);
        lv_obj_align(rssi_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    (void)g; /* peers are display-only for now */
    free(mesh);
}

/* ── Screen entry point ──────────────────────────────────────────────── */

void scr_settings_create(bramble_layout_t* layout) {
    lv_obj_t* cont = layout_get_content(layout);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, BR_PADDING, 0);
    lv_obj_set_style_pad_row(cont, 4, 0);

    lv_group_t* g = lv_group_get_default();

    /* ── Title ── */
    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);

    /* ── Node Name (editable) ── */
    lv_obj_t* name_row = create_setting_row(cont, "Node Name");
    const char* node_name = mesh_get_node_name();
    s_name_label = lv_label_create(name_row);
    lv_label_set_text(s_name_label, node_name ? node_name : "(not set)");
    lv_obj_set_style_text_color(s_name_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_name_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(name_row, name_edit_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, name_row);

    /* ── Node Identity QR share ── */
    lv_obj_t* identity_qr_row = create_setting_row(cont, "Share Identity QR");
    lv_obj_t* identity_qr_hint = lv_label_create(identity_qr_row);
    lv_label_set_text(identity_qr_hint, "QR");
    lv_obj_set_style_text_color(identity_qr_hint, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(identity_qr_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(identity_qr_hint, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(identity_qr_row, identity_qr_open_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, identity_qr_row);

    /* ── Keyboard Backlight slider ── */
    uint8_t cur_backlight = keyboard_get_backlight_percent();

    lv_obj_t* bl_row = create_setting_row(cont, "Backlight");
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

    lv_obj_t* vol_row = create_setting_row(cont, LV_SYMBOL_AUDIO " Volume");
    lv_obj_set_size(vol_row, 304, 48);
    s_volume_slider = lv_slider_create(vol_row);
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
    lv_obj_t* mute_row = create_setting_row(cont, LV_SYMBOL_MUTE " Silent");
    s_mute_sw = lv_switch_create(mute_row);
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

    lv_obj_t* sleep_row = create_setting_row(cont, LV_SYMBOL_EYE_CLOSE " Auto-Sleep");
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
    lv_obj_t* sleep_timeout_row = create_setting_row(cont, "Timeout");
    lv_obj_set_size(sleep_timeout_row, 304, 48);

    /* Value label showing "XXs" */
    s_sleep_timeout_label = lv_label_create(sleep_timeout_row);
    char timeout_buf[16];
    snprintf(timeout_buf, sizeof(timeout_buf), "%us", cur_sleep_timeout);
    lv_label_set_text(s_sleep_timeout_label, timeout_buf);
    lv_obj_set_style_text_color(s_sleep_timeout_label, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_sleep_timeout_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_sleep_timeout_label, LV_ALIGN_RIGHT_MID, -150, 0);

    s_sleep_timeout_slider = lv_slider_create(sleep_timeout_row);
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

    /* ── Location Sharing ── */
    location_ui_load_state(&s_loc_state);
    location_contacts_load();

    lv_obj_t* sep_loc = lv_obj_create(cont);
    lv_obj_set_size(sep_loc, 296, 1);
    lv_obj_set_style_bg_color(sep_loc, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep_loc, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep_loc, 0, 0);

    lv_obj_t* loc_section_lbl = lv_label_create(cont);
    lv_label_set_text(loc_section_lbl, LV_SYMBOL_GPS " Location Sharing");
    lv_obj_set_style_text_font(loc_section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_section_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t* loc_share_row = create_setting_row(cont, "Share Location");
    s_loc_share_sw = lv_switch_create(loc_share_row);
    lv_obj_align(s_loc_share_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    if (s_loc_state.sharing_enabled)
        lv_obj_add_state(s_loc_share_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_loc_share_sw, location_share_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_loc_share_sw);

    lv_obj_t* loc_tier_row = create_setting_row(cont, "Privacy Tier");
    s_loc_tier_dd = lv_dropdown_create(loc_tier_row);
    lv_dropdown_set_options(s_loc_tier_dd, "Coarse\nExact\nPresence");
    lv_obj_set_size(s_loc_tier_dd, 130, 34);
    lv_obj_align(s_loc_tier_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    uint16_t tier_idx = 0;
    if (s_loc_state.tier == LOCATION_UI_TIER_FULL)
        tier_idx = 1;
    else if (s_loc_state.tier == LOCATION_UI_TIER_PRESENCE)
        tier_idx = 2;
    lv_dropdown_set_selected(s_loc_tier_dd, tier_idx);
    lv_obj_add_event_cb(s_loc_tier_dd, location_tier_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_loc_tier_dd);

    lv_obj_t* loc_interval_row = create_setting_row(cont, "Interval");
    s_loc_interval_dd = lv_dropdown_create(loc_interval_row);
    lv_dropdown_set_options(s_loc_interval_dd, "1 min\n5 min\n15 min\n60 min");
    lv_obj_set_size(s_loc_interval_dd, 130, 34);
    lv_obj_align(s_loc_interval_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(s_loc_interval_dd,
                             location_interval_to_dropdown(s_loc_state.interval_s));
    lv_obj_add_event_cb(s_loc_interval_dd, location_interval_changed_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    if (g)
        lv_group_add_obj(g, s_loc_interval_dd);

    lv_obj_t* loc_source_row = create_setting_row(cont, "Active Source");
    s_loc_source_lbl = lv_label_create(loc_source_row);
    lv_obj_set_style_text_color(s_loc_source_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_loc_source_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_loc_source_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* loc_last_row = create_setting_row(cont, "Last Share");
    s_loc_last_share_lbl = lv_label_create(loc_last_row);
    lv_obj_set_style_text_color(s_loc_last_share_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_loc_last_share_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_loc_last_share_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    location_refresh_status_labels();

    lv_obj_t* panic_btn = lv_btn_create(cont);
    lv_obj_set_size(panic_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(panic_btn, BR_COLOR_DANGER, 0);
    lv_obj_set_style_radius(panic_btn, BR_RADIUS, 0);
    lv_obj_t* panic_lbl = lv_label_create(panic_btn);
    lv_label_set_text(panic_lbl, LV_SYMBOL_WARNING " Panic Off Location Sharing");
    lv_obj_set_style_text_font(panic_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(panic_lbl);
    lv_obj_add_event_cb(panic_btn, location_panic_off_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, panic_btn);

    /* ── Peer Targets ── */
    build_loc_peer_targets_section(cont, g);

    /* ── Connectivity Mode ── */
    {
        lv_obj_t* sep_conn = lv_obj_create(cont);
        lv_obj_set_size(sep_conn, 296, 1);
        lv_obj_set_style_bg_color(sep_conn, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_bg_opa(sep_conn, LV_OPA_30, 0);
        lv_obj_set_style_border_width(sep_conn, 0, 0);

        lv_obj_t* conn_section_lbl = lv_label_create(cont);
        lv_label_set_text(conn_section_lbl, LV_SYMBOL_WIFI " Connectivity Mode");
        lv_obj_set_style_text_font(conn_section_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(conn_section_lbl, BR_COLOR_TEXT, 0);

        /* Dropdown selector row */
        lv_obj_t* conn_row = create_setting_row(cont, "Mode");
        lv_obj_set_size(conn_row, 304, 48);

        s_conn_dropdown = lv_dropdown_create(conn_row);
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

    /* ── Radio Configuration ── */
    build_radio_config_section(cont, g);

    /* ── Channel Manager ── */
    {
        lv_obj_t* sep_ch = lv_obj_create(cont);
        lv_obj_set_size(sep_ch, 296, 1);
        lv_obj_set_style_bg_color(sep_ch, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_bg_opa(sep_ch, LV_OPA_30, 0);
        lv_obj_set_style_border_width(sep_ch, 0, 0);

        /* Container for channel list (cleaned + rebuilt on add/remove) */
        lv_obj_t* ch_cont = lv_obj_create(cont);
        lv_obj_set_width(ch_cont, 308);
        lv_obj_set_height(ch_cont, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(ch_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ch_cont, 0, 0);
        lv_obj_set_style_pad_all(ch_cont, 0, 0);
        lv_obj_set_flex_flow(ch_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(ch_cont, 4, 0);
        build_channel_manager_section(ch_cont, g);
    }

    /* ── Peer Manager ── */
    build_peer_manager_section(cont, g);

    /* ── Board info ── */
    lv_obj_t* board_row = create_setting_row(cont, "Board");
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
    lv_obj_t* ver_row = create_setting_row(cont, "Version");
    lv_obj_t* ver_val = lv_label_create(ver_row);
    lv_label_set_text(ver_val, esp_app_get_description()->version);
    lv_obj_set_style_text_color(ver_val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(ver_val, &lv_font_montserrat_12, 0);
    lv_obj_align(ver_val, LV_ALIGN_RIGHT_MID, 0, 0);

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
