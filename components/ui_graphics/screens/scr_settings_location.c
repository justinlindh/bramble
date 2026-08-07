#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_confirm.h"
#include "ui_toast.h"
#include "ui_zone.h"
#include "location_settings_ui.h"
#include "location.h"
#include "routing.h"
#include "airtime_budget.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "scr_set_loc";

/* Mesh state snapshot: mirrored from mesh_task.h to avoid an include cycle.
 * Only the neighbor table is read here (for peer target rows). */
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

/* ── Location sharing controls ───────────────────────────────────────────── */

static lv_obj_t* s_loc_share_sw = NULL;
static lv_obj_t* s_loc_tier_dd = NULL;
static lv_obj_t* s_loc_interval_dd = NULL;
static lv_obj_t* s_loc_source_lbl = NULL;
static lv_obj_t* s_loc_last_share_lbl = NULL;
/* Shown only while sharing is switched on with nothing to send to. */
static lv_obj_t* s_loc_no_target_lbl = NULL;
static location_ui_state_t s_loc_state;

/* ── Location peer targets ───────────────────────────────────────────────── */

/*
 * Peer targets are the canonical location-share rules: one NVS key per peer,
 * LOCATION_CONTACT_RULE_PREFIX plus the address, holding the shared rule
 * string. This is the same storage bramble.setLocationConfig writes and the
 * only storage the send path reads, so a target added here transmits and
 * shows up in getConfig, and one added from the app shows up here.
 */
typedef struct {
    uint32_t addr;
    bool enabled;
} loc_contact_ui_t;

#define LOC_CONTACT_UI_MAX LOCATION_MAX_CONTACTS /* 16 */

static loc_contact_ui_t s_loc_contacts[LOC_CONTACT_UI_MAX];
static int s_loc_contact_count = 0;
/* Channel targets are configured from the app, not from this screen, but they
 * are targets, so the summary has to count them or it reports "no targets" on
 * a node that is sharing to a channel. */
static int s_loc_channel_target_count = 0;

static int location_contacts_find(uint32_t addr) {
    for (int i = 0; i < s_loc_contact_count; i++) {
        if (s_loc_contacts[i].addr == addr)
            return i;
    }
    return -1;
}

/*
 * One-time carry-over of the legacy "ct_data" blob.
 *
 * That blob was private to this screen: nothing else read it, so peers
 * toggled on here were never transmitted to and never appeared in the app.
 * Rewriting each record as a canonical rule key makes those selections real,
 * and the blob is erased so this runs once.
 */
static void location_contacts_migrate_legacy_blob(void) {
    typedef struct {
        uint32_t addr;
        uint8_t tier; /* LOCATION_UI_TIER_* : 0xFF = use global */
        uint8_t enabled;
    } legacy_contact_t;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK)
        return;

    legacy_contact_t legacy[LOC_CONTACT_UI_MAX];
    size_t len = sizeof(legacy);
    if (nvs_get_blob(nvs, "ct_data", legacy, &len) != ESP_OK || len == 0 ||
        (len % sizeof(legacy_contact_t)) != 0) {
        nvs_close(nvs);
        return;
    }

    location_policy_t policy;
    location_policy_set_defaults(&policy);
    uint16_t interval_s = 0;
    if (nvs_get_u16(nvs, "interval_s", &interval_s) == ESP_OK)
        policy.interval_s = interval_s;
    char tier_buf[16] = {0};
    size_t tier_len = sizeof(tier_buf);
    if (nvs_get_str(nvs, "def_tier", tier_buf, &tier_len) == ESP_OK)
        policy.default_tier = location_tier_from_string(tier_buf);
    location_policy_normalize(&policy);

    int count = (int)(len / sizeof(legacy_contact_t));
    if (count > LOC_CONTACT_UI_MAX)
        count = LOC_CONTACT_UI_MAX;
    for (int i = 0; i < count; i++) {
        char key[LOCATION_TARGET_KEY_SIZE];
        if (!location_contact_key(key, sizeof(key), legacy[i].addr))
            continue;
        location_rule_t rule = {
            .enabled = legacy[i].enabled != 0,
            .tier = legacy[i].tier == 0xFF ? policy.default_tier : legacy[i].tier,
            .interval_s = policy.interval_s,
        };
        char value[48];
        location_rule_format(value, sizeof(value), &rule);
        nvs_set_str(nvs, key, value);
    }

    nvs_erase_key(nvs, "ct_data");
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "carried %d peer target(s) over to canonical location rules", count);
}

static void location_contacts_load(void) {
    location_contacts_migrate_legacy_blob();

    s_loc_contact_count = 0;
    s_loc_channel_target_count = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK)
        return;

    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) == ESP_OK) {
        while (it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            char raw[64] = {0};
            size_t raw_len = sizeof(raw);
            if (nvs_get_str(nvs, info.key, raw, &raw_len) == ESP_OK) {
                location_rule_t rule = {
                    .enabled = true,
                    .tier = LOCATION_TIER_COARSE,
                    .interval_s = LOCATION_DEFAULT_INTERVAL_S,
                };
                if (strncmp(info.key, LOCATION_CONTACT_RULE_PREFIX,
                            sizeof(LOCATION_CONTACT_RULE_PREFIX) - 1) == 0) {
                    location_rule_parse(raw, &rule);
                    if (s_loc_contact_count < LOC_CONTACT_UI_MAX) {
                        s_loc_contacts[s_loc_contact_count].addr = (uint32_t)strtoul(
                            info.key + sizeof(LOCATION_CONTACT_RULE_PREFIX) - 1, NULL, 16);
                        s_loc_contacts[s_loc_contact_count].enabled = rule.enabled;
                        s_loc_contact_count++;
                    }
                } else if (strncmp(info.key, LOCATION_CHANNEL_RULE_PREFIX,
                                   sizeof(LOCATION_CHANNEL_RULE_PREFIX) - 1) == 0) {
                    location_rule_parse(raw, &rule);
                    if (rule.enabled)
                        s_loc_channel_target_count++;
                }
            }

            if (nvs_entry_next(&it) != ESP_OK) {
                break;
            }
        }
        nvs_release_iterator(it);
    }

    nvs_close(nvs);
}

static bool location_contact_is_enabled(uint32_t addr) {
    int idx = location_contacts_find(addr);
    return idx >= 0 && s_loc_contacts[idx].enabled;
}

static int location_enabled_target_count(void) {
    int count = s_loc_channel_target_count;
    for (int i = 0; i < s_loc_contact_count; i++) {
        if (s_loc_contacts[i].enabled)
            count++;
    }
    return count;
}

/* The share switch is a permission, not an activity: with it on and no target
 * configured the node transmits nothing. Say so on the screen rather than
 * leaving a switch that reads as "sharing". */
static void location_share_hint_refresh(void) {
    if (!s_loc_no_target_lbl)
        return;
    if (s_loc_state.sharing_enabled && location_enabled_target_count() == 0) {
        lv_label_set_text(s_loc_no_target_lbl, "No targets yet, so nothing is sent.\n"
                                               "Pick a peer below, or a channel in the app.");
        lv_obj_clear_flag(s_loc_no_target_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_loc_no_target_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Toggle callback: user_data encodes peer addr as uintptr_t. Writing the rule
 * key straight through is what makes the toggle mean something: the send path
 * picks it up on its next policy tick. */
static void location_contact_toggle_cb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool want_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint32_t addr = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    char key[LOCATION_TARGET_KEY_SIZE];
    if (!location_contact_key(key, sizeof(key), addr))
        return;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK)
        return;

    if (want_enabled) {
        location_rule_t rule = {
            .enabled = true,
            .tier = (uint8_t)s_loc_state.tier,
            .interval_s = s_loc_state.interval_s,
        };
        char value[48];
        location_rule_format(value, sizeof(value), &rule);
        nvs_set_str(nvs, key, value);
    } else {
        nvs_erase_key(nvs, key);
    }
    nvs_commit(nvs);
    nvs_close(nvs);

    int idx = location_contacts_find(addr);
    if (want_enabled) {
        if (idx >= 0) {
            s_loc_contacts[idx].enabled = true;
        } else if (s_loc_contact_count < LOC_CONTACT_UI_MAX) {
            s_loc_contacts[s_loc_contact_count].addr = addr;
            s_loc_contacts[s_loc_contact_count].enabled = true;
            s_loc_contact_count++;
        }
    } else if (idx >= 0) {
        for (int i = idx; i < s_loc_contact_count - 1; i++) {
            s_loc_contacts[i] = s_loc_contacts[i + 1];
        }
        s_loc_contact_count--;
    }

    location_share_hint_refresh();
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
    location_share_hint_refresh();
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

/* ── Peer targets section builder ────────────────────────────────────────── */

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

    /* Get current neighbor snapshot (heap-allocated: too large for 8KB task stack) */
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

        /* Pre-check if this peer is already an enabled target */
        if (location_contact_is_enabled(nb->addr)) {
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
        const loc_contact_ui_t* ct = &s_loc_contacts[ci];
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

/* ── Subpage ─────────────────────────────────────────────────────────────── */

void settings_location_summary(char* buf, size_t n) {
    location_ui_state_t st;
    location_ui_load_state(&st);
    if (!st.sharing_enabled) {
        snprintf(buf, n, "Off");
        return;
    }
    location_contacts_load();
    const char* tier = "coarse";
    if (st.tier == LOCATION_UI_TIER_FULL)
        tier = "exact";
    else if (st.tier == LOCATION_UI_TIER_PRESENCE)
        tier = "presence";
    /* Count only ENABLED targets: a rule the app switched off is a record,
     * not an audience, and counting records reported "16 peers" on a bench
     * with two peers, both switched off. */
    int enabled_contacts = 0;
    for (int i = 0; i < s_loc_contact_count; i++) {
        if (s_loc_contacts[i].enabled)
            enabled_contacts++;
    }
    location_ui_format_share_summary(buf, n, st.sharing_enabled, enabled_contacts,
                                     s_loc_channel_target_count, tier);
}

void settings_location_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_t* cont = settings_subpage_begin(layout, "Location");
    lv_group_t* g = lv_group_get_default();

    location_ui_load_state(&s_loc_state);
    location_contacts_load();

    lv_obj_t* loc_share_row = settings_create_setting_row(cont, "Share Location");
    ui_zone_track(&s_loc_share_sw, lv_switch_create(loc_share_row));
    lv_obj_align(s_loc_share_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_loc_share_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    if (s_loc_state.sharing_enabled)
        lv_obj_add_state(s_loc_share_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_loc_share_sw, location_share_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, s_loc_share_sw);

    ui_zone_track(&s_loc_no_target_lbl, lv_label_create(cont));
    lv_obj_set_style_text_font(s_loc_no_target_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_loc_no_target_lbl, BR_COLOR_WARNING, 0);
    location_share_hint_refresh();

    lv_obj_t* loc_tier_row = settings_create_setting_row(cont, "Privacy Tier");
    ui_zone_track(&s_loc_tier_dd, lv_dropdown_create(loc_tier_row));
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

    lv_obj_t* loc_interval_row = settings_create_setting_row(cont, "Interval");
    ui_zone_track(&s_loc_interval_dd, lv_dropdown_create(loc_interval_row));
    lv_dropdown_set_options(s_loc_interval_dd, "1 min\n5 min\n15 min\n60 min");
    lv_obj_set_size(s_loc_interval_dd, 130, 34);
    lv_obj_align(s_loc_interval_dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(s_loc_interval_dd,
                             location_interval_to_dropdown(s_loc_state.interval_s));
    lv_obj_add_event_cb(s_loc_interval_dd, location_interval_changed_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    if (g)
        lv_group_add_obj(g, s_loc_interval_dd);

    lv_obj_t* loc_source_row = settings_create_setting_row(cont, "Active Source");
    ui_zone_track(&s_loc_source_lbl, lv_label_create(loc_source_row));
    lv_obj_set_style_text_color(s_loc_source_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_loc_source_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_loc_source_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* loc_last_row = settings_create_setting_row(cont, "Last Share");
    ui_zone_track(&s_loc_last_share_lbl, lv_label_create(loc_last_row));
    lv_obj_set_style_text_color(s_loc_last_share_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_loc_last_share_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_loc_last_share_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    location_refresh_status_labels();

    /* ── Peer Targets ── */
    build_loc_peer_targets_section(cont, g);

    /* Panic Off sits at the BOTTOM of the page, away from the routine toggles
     * above, keeping its red styling and confirm dialog. */
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
}
