#include "scr_settings.h"
#include "theme/bramble_theme.h"
#include "display.h"
#include "keyboard.h"
#include "audio.h"
#include "sleep_manager.h"
#include "board_config.h"
#include "ui.h"
#include "location_settings_ui.h"
#include "location.h"
#include "routing.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "scr_settings";

/* Get/set node name from mesh — extern since it's in main */
extern int mesh_set_node_name_persist(const char *name);
extern const char *mesh_get_node_name(void);

/* Mesh state snapshot — mirrored from mesh_task.h to avoid include cycle */
typedef struct {
    neighbor_table_t neighbors;
    uint32_t         beacon_tx_count;
    uint32_t         beacon_rx_count;
    uint32_t         packets_tx;
    uint32_t         packets_rx;
    bool             radio_ok;
    int16_t          last_rx_rssi;
    int8_t           last_rx_snr;
} settings_mesh_state_t;
extern void mesh_get_state(settings_mesh_state_t *out);

/* ── Backlight ───────────────────────────────────────────────────────── */

static void backlight_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);  /* 0..100 */
    /* Persist backlight value to NVS and apply to hardware */
    keyboard_set_backlight_percent((uint8_t)val);
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

/* ── Sleep mode ─────────────────────────────────────────────────────── */

static lv_obj_t *s_sleep_timeout_slider = NULL;
static lv_obj_t *s_sleep_timeout_label = NULL;

static void sleep_enabled_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
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

static void sleep_timeout_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);  /* 10..300 seconds */
    sleep_manager_set_timeout((uint16_t)val);
    
    /* Update the value label */
    if (s_sleep_timeout_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ds", val);
        lv_label_set_text(s_sleep_timeout_label, buf);
    }
}

/* ── Location sharing controls ───────────────────────────────────────── */

static lv_obj_t *s_loc_share_sw = NULL;
static lv_obj_t *s_loc_tier_dd = NULL;
static lv_obj_t *s_loc_interval_dd = NULL;
static lv_obj_t *s_loc_source_lbl = NULL;
static lv_obj_t *s_loc_last_share_lbl = NULL;
static location_ui_state_t s_loc_state;

/* ── Location peer targets ───────────────────────────────────────────── */

/* Per-peer contact record stored in NVS blob "ct_data" under "bramble_loc" */
typedef struct {
    uint32_t addr;
    uint8_t  tier;     /* LOCATION_UI_TIER_* — 0xFF = use global */
    uint8_t  enabled;
} loc_contact_nvs_t;

#define LOC_CONTACT_UI_MAX  LOCATION_MAX_CONTACTS  /* 16 */

static loc_contact_nvs_t s_loc_contacts[LOC_CONTACT_UI_MAX];
static int               s_loc_contact_count = 0;

static int location_contacts_find(uint32_t addr) {
    for (int i = 0; i < s_loc_contact_count; i++) {
        if (s_loc_contacts[i].addr == addr) return i;
    }
    return -1;
}

static void location_contacts_load(void) {
    s_loc_contact_count = 0;
    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) != ESP_OK) return;
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
    if (nvs_open("bramble_loc", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, "ct_data", s_loc_contacts,
                 (size_t)(s_loc_contact_count) * sizeof(loc_contact_nvs_t));
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Toggle callback — user_data encodes peer addr as uintptr_t */
static void location_contact_toggle_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool want_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint32_t addr = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    int idx = location_contacts_find(addr);
    if (want_enabled) {
        if (idx < 0) {
            /* Add new entry if space available */
            if (s_loc_contact_count < LOC_CONTACT_UI_MAX) {
                s_loc_contacts[s_loc_contact_count].addr    = addr;
                s_loc_contacts[s_loc_contact_count].tier    = 0xFF; /* use global */
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
    ESP_LOGI(TAG, "Peer target %08lX %s", (unsigned long)addr,
             want_enabled ? "added" : "removed");
}

static void location_ui_load_state(location_ui_state_t *st) {
    if (!st) return;

    st->sharing_enabled = false;
    st->tier = LOCATION_UI_TIER_COARSE;
    st->interval_s = LOCATION_UI_INTERVAL_5_MIN;
    st->source = LOCATION_UI_SOURCE_HYBRID;
    st->last_share_epoch_s = 0;

    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) != ESP_OK) return;

    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) st->sharing_enabled = (enabled != 0);

    uint16_t interval_s = 0;
    if (nvs_get_u16(nvs, "interval_s", &interval_s) == ESP_OK) {
        if (interval_s == LOCATION_UI_INTERVAL_1_MIN || interval_s == LOCATION_UI_INTERVAL_5_MIN ||
            interval_s == LOCATION_UI_INTERVAL_15_MIN || interval_s == LOCATION_UI_INTERVAL_60_MIN) {
            st->interval_s = interval_s;
        }
    }

    char tier[16] = {0};
    size_t tier_len = sizeof(tier);
    if (nvs_get_str(nvs, "def_tier", tier, &tier_len) == ESP_OK) {
        if (strcmp(tier, "exact") == 0 || strcmp(tier, "full") == 0) st->tier = LOCATION_UI_TIER_FULL;
        else if (strcmp(tier, "presence") == 0) st->tier = LOCATION_UI_TIER_PRESENCE;
        else st->tier = LOCATION_UI_TIER_COARSE;
    }

    char source[16] = {0};
    size_t source_len = sizeof(source);
    if (nvs_get_str(nvs, "source", source, &source_len) == ESP_OK) {
        if (strcmp(source, "gps") == 0) st->source = LOCATION_UI_SOURCE_GPS;
        else if (strcmp(source, "manual") == 0) st->source = LOCATION_UI_SOURCE_MANUAL;
        else st->source = LOCATION_UI_SOURCE_HYBRID;
    }

    uint32_t last_share = 0;
    if (nvs_get_u32(nvs, "last_share_s", &last_share) == ESP_OK) st->last_share_epoch_s = last_share;

    nvs_close(nvs);
}

static void location_ui_save_state(const location_ui_state_t *st) {
    if (!st) return;
    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_set_u8(nvs, "enabled", st->sharing_enabled ? 1 : 0);
    nvs_set_u16(nvs, "interval_s", st->interval_s);

    const char *tier = "coarse";
    if (st->tier == LOCATION_UI_TIER_FULL) tier = "exact";
    else if (st->tier == LOCATION_UI_TIER_PRESENCE) tier = "presence";
    nvs_set_str(nvs, "def_tier", tier);

    const char *source = "hybrid";
    if (st->source == LOCATION_UI_SOURCE_GPS) source = "gps";
    else if (st->source == LOCATION_UI_SOURCE_MANUAL) source = "manual";
    nvs_set_str(nvs, "source", source);

    nvs_commit(nvs);
    nvs_close(nvs);
}

static uint16_t location_interval_from_dropdown(uint16_t selected) {
    switch (selected) {
        case 0: return LOCATION_UI_INTERVAL_1_MIN;
        case 1: return LOCATION_UI_INTERVAL_5_MIN;
        case 2: return LOCATION_UI_INTERVAL_15_MIN;
        case 3: return LOCATION_UI_INTERVAL_60_MIN;
        default: return LOCATION_UI_INTERVAL_5_MIN;
    }
}

static uint16_t location_interval_to_dropdown(uint16_t interval_s) {
    switch (interval_s) {
        case LOCATION_UI_INTERVAL_1_MIN: return 0;
        case LOCATION_UI_INTERVAL_5_MIN: return 1;
        case LOCATION_UI_INTERVAL_15_MIN: return 2;
        case LOCATION_UI_INTERVAL_60_MIN: return 3;
        default: return 1;
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

static void location_share_changed_cb(lv_event_t *e) {
    bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_SET_SHARING, enabled ? 1 : 0);
    location_ui_save_state(&s_loc_state);
}

static void location_tier_changed_cb(lv_event_t *e) {
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    location_ui_tier_t tier = LOCATION_UI_TIER_COARSE;
    if (sel == 0) tier = LOCATION_UI_TIER_COARSE;
    else if (sel == 1) tier = LOCATION_UI_TIER_FULL;
    else if (sel == 2) tier = LOCATION_UI_TIER_PRESENCE;
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_SET_TIER, tier);
    location_ui_save_state(&s_loc_state);
}

static void location_interval_changed_cb(lv_event_t *e) {
    uint16_t interval_s = location_interval_from_dropdown(lv_dropdown_get_selected(lv_event_get_target(e)));
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_SET_INTERVAL, interval_s);
    location_ui_save_state(&s_loc_state);
}

static void location_panic_off_cb(lv_event_t *e) {
    (void)e;
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_PANIC_OFF, 0);
    location_ui_save_state(&s_loc_state);
    if (s_loc_share_sw) lv_obj_clear_state(s_loc_share_sw, LV_STATE_CHECKED);
}

/* ── Peer targets section builder ───────────────────────────────────── */

static void build_loc_peer_targets_section(lv_obj_t *cont, lv_group_t *g) {
    /* Section separator */
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    lv_obj_t *section_lbl = lv_label_create(cont);
    lv_label_set_text(section_lbl, LV_SYMBOL_CALL " Peer Targets");
    lv_obj_set_style_text_font(section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t *hint = lv_label_create(cont);
    lv_label_set_text(hint, "Send location to specific peers.\n"
                            "Uses global tier unless overridden.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);

    /* Get current neighbor snapshot */
    settings_mesh_state_t mesh;
    mesh_get_state(&mesh);
    int n_count = neighbor_count(&mesh.neighbors);

    if (n_count == 0) {
        lv_obj_t *no_peers = lv_label_create(cont);
        lv_label_set_text(no_peers, "(no peers visible)");
        lv_obj_set_style_text_font(no_peers, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(no_peers, BR_COLOR_TEXT_SEC, 0);
    }

    /* Render one toggle row per neighbor entry */
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const neighbor_entry_t *nb = &mesh.neighbors.entries[i];
        if (nb->addr == 0) continue;  /* empty slot */

        /* Row container */
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Label: name or hex addr */
        lv_obj_t *lbl = lv_label_create(row);
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
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sw, BR_COLOR_TEXT, LV_PART_KNOB);

        /* Pre-check if this peer is already a target */
        if (location_contacts_find(nb->addr) >= 0) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }

        lv_obj_add_event_cb(sw, location_contact_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)nb->addr);
        if (g) lv_group_add_obj(g, sw);
    }

    /* Show persisted offline targets (not currently visible as neighbors) */
    bool shown_offline_hdr = false;
    for (int ci = 0; ci < s_loc_contact_count; ci++) {
        const loc_contact_nvs_t *ct = &s_loc_contacts[ci];
        /* Check if already shown as a neighbor */
        bool is_neighbor = false;
        for (int ni = 0; ni < MAX_NEIGHBORS; ni++) {
            if (mesh.neighbors.entries[ni].addr == ct->addr) {
                is_neighbor = true;
                break;
            }
        }
        if (is_neighbor) continue;

        if (!shown_offline_hdr) {
            lv_obj_t *off_lbl = lv_label_create(cont);
            lv_label_set_text(off_lbl, "Saved (offline):");
            lv_obj_set_style_text_font(off_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(off_lbl, BR_COLOR_TEXT_SEC, 0);
            shown_offline_hdr = true;
        }

        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char hex[12];
        snprintf(hex, sizeof(hex), "%08lX", (unsigned long)ct->addr);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, hex);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_width(lbl, 200);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sw, BR_COLOR_TEXT, LV_PART_KNOB);
        if (ct->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);

        lv_obj_add_event_cb(sw, location_contact_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)ct->addr);
        if (g) lv_group_add_obj(g, sw);
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
    lv_obj_set_style_bg_color(s_name_edit_overlay, BR_COLOR_BG, 0);
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
    lv_textarea_set_max_length(s_name_edit_ta, 32);
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
    uint8_t cur_backlight = keyboard_get_backlight_percent();

    lv_obj_t *bl_row = create_setting_row(cont, "Backlight");
    lv_obj_set_size(bl_row, 304, 48);
    lv_obj_t *bl_slider = lv_slider_create(bl_row);
    lv_obj_set_size(bl_slider, 140, 10);
    lv_obj_align(bl_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(bl_slider, 0, 100);
    lv_slider_set_value(bl_slider, cur_backlight, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bl_slider, BR_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(bl_slider, backlight_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, bl_slider);

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
    lv_obj_set_style_bg_color(s_volume_slider, BR_COLOR_SURFACE_2, LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_mute_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    /* Reflect saved mute state */
    if (cur_muted) {
        lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_mute_sw, mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, s_mute_sw);

    /* ── Sleep mode ── */
    bool cur_sleep_enabled = sleep_manager_get_enabled();
    uint16_t cur_sleep_timeout = sleep_manager_get_timeout();

    lv_obj_t *sleep_row = create_setting_row(cont, LV_SYMBOL_EYE_CLOSE " Auto-Sleep");
    lv_obj_t *sleep_sw = lv_switch_create(sleep_row);
    lv_obj_align(sleep_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sleep_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sleep_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sleep_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    if (cur_sleep_enabled) {
        lv_obj_add_state(sleep_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sleep_sw, sleep_enabled_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, sleep_sw);

    /* Sleep timeout slider */
    lv_obj_t *sleep_timeout_row = create_setting_row(cont, "Timeout");
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
    lv_obj_add_event_cb(s_sleep_timeout_slider, sleep_timeout_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Dim slider and label if sleep is disabled */
    if (!cur_sleep_enabled) {
        lv_obj_set_style_opa(s_sleep_timeout_slider, LV_OPA_40, 0);
        lv_obj_set_style_opa(s_sleep_timeout_label, LV_OPA_40, 0);
    }
    if (g) lv_group_add_obj(g, s_sleep_timeout_slider);

    /* ── Location Sharing ── */
    location_ui_load_state(&s_loc_state);
    location_contacts_load();

    lv_obj_t *sep_loc = lv_obj_create(cont);
    lv_obj_set_size(sep_loc, 296, 1);
    lv_obj_set_style_bg_color(sep_loc, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep_loc, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep_loc, 0, 0);

    lv_obj_t *loc_section_lbl = lv_label_create(cont);
    lv_label_set_text(loc_section_lbl, LV_SYMBOL_GPS " Location Sharing");
    lv_obj_set_style_text_font(loc_section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_section_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t *loc_share_row = create_setting_row(cont, "Share Location");
    s_loc_share_sw = lv_switch_create(loc_share_row);
    lv_obj_align(s_loc_share_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    if (s_loc_state.sharing_enabled) lv_obj_add_state(s_loc_share_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_loc_share_sw, location_share_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, s_loc_share_sw);

    lv_obj_t *loc_tier_row = create_setting_row(cont, "Privacy Tier");
    s_loc_tier_dd = lv_dropdown_create(loc_tier_row);
    lv_dropdown_set_options(s_loc_tier_dd, "Coarse\nExact\nPresence");
    lv_obj_set_size(s_loc_tier_dd, 130, 34);
    lv_obj_align(s_loc_tier_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    uint16_t tier_idx = 0;
    if (s_loc_state.tier == LOCATION_UI_TIER_FULL) tier_idx = 1;
    else if (s_loc_state.tier == LOCATION_UI_TIER_PRESENCE) tier_idx = 2;
    lv_dropdown_set_selected(s_loc_tier_dd, tier_idx);
    lv_obj_add_event_cb(s_loc_tier_dd, location_tier_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, s_loc_tier_dd);

    lv_obj_t *loc_interval_row = create_setting_row(cont, "Interval");
    s_loc_interval_dd = lv_dropdown_create(loc_interval_row);
    lv_dropdown_set_options(s_loc_interval_dd, "1 min\n5 min\n15 min\n60 min");
    lv_obj_set_size(s_loc_interval_dd, 130, 34);
    lv_obj_align(s_loc_interval_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(s_loc_interval_dd, location_interval_to_dropdown(s_loc_state.interval_s));
    lv_obj_add_event_cb(s_loc_interval_dd, location_interval_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g) lv_group_add_obj(g, s_loc_interval_dd);

    lv_obj_t *loc_source_row = create_setting_row(cont, "Active Source");
    s_loc_source_lbl = lv_label_create(loc_source_row);
    lv_obj_set_style_text_color(s_loc_source_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_loc_source_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_loc_source_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *loc_last_row = create_setting_row(cont, "Last Share");
    s_loc_last_share_lbl = lv_label_create(loc_last_row);
    lv_obj_set_style_text_color(s_loc_last_share_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_loc_last_share_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_loc_last_share_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    location_refresh_status_labels();

    lv_obj_t *panic_btn = lv_btn_create(cont);
    lv_obj_set_size(panic_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(panic_btn, BR_COLOR_DANGER, 0);
    lv_obj_set_style_radius(panic_btn, BR_RADIUS, 0);
    lv_obj_t *panic_lbl = lv_label_create(panic_btn);
    lv_label_set_text(panic_lbl, LV_SYMBOL_WARNING " Panic Off Location Sharing");
    lv_obj_set_style_text_font(panic_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(panic_lbl);
    lv_obj_add_event_cb(panic_btn, location_panic_off_cb, LV_EVENT_CLICKED, NULL);
    if (g) lv_group_add_obj(g, panic_btn);

    /* ── Peer Targets ── */
    build_loc_peer_targets_section(cont, g);

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
            "BLE only");
        lv_obj_set_size(s_conn_dropdown, 150, 34);
        lv_obj_align(s_conn_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

        /* Style to match dark theme */
        lv_obj_set_style_bg_color(s_conn_dropdown, BR_COLOR_SURFACE_2, 0);
        lv_obj_set_style_text_color(s_conn_dropdown, BR_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(s_conn_dropdown, &lv_font_montserrat_12, 0);
        lv_obj_set_style_border_color(s_conn_dropdown, BR_COLOR_PRIMARY, 0);
        lv_obj_set_style_border_width(s_conn_dropdown, 1, 0);

        lv_obj_t *dd_list = lv_dropdown_get_list(s_conn_dropdown);
        lv_obj_set_style_bg_color(dd_list, BR_COLOR_SURFACE_2, 0);
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
            "Modes are exclusive");
        lv_obj_set_style_text_font(conn_hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(conn_hint, BR_COLOR_TEXT_SEC, 0);

        /* Apply & Reboot button */
        lv_obj_t *apply_btn = lv_btn_create(cont);
        lv_obj_set_size(apply_btn, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(apply_btn, BR_COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0x1A6628), LV_STATE_PRESSED);  /* Darker green */
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
