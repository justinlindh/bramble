#ifndef LOCATION_SETTINGS_UI_H
#define LOCATION_SETTINGS_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* UI-friendly location control constants for Settings screen + host tests. */
typedef enum {
    LOCATION_UI_TIER_COARSE = 1,
    LOCATION_UI_TIER_FULL = 0,
    LOCATION_UI_TIER_PRESENCE = 2,
} location_ui_tier_t;

typedef enum {
    LOCATION_UI_SOURCE_HYBRID = 0,
    LOCATION_UI_SOURCE_GPS = 1,
    LOCATION_UI_SOURCE_MANUAL = 2,
} location_ui_source_t;

typedef enum {
    LOCATION_UI_ACTION_SET_SHARING = 0,
    LOCATION_UI_ACTION_SET_TIER,
    LOCATION_UI_ACTION_SET_INTERVAL,
    LOCATION_UI_ACTION_SET_SOURCE,
    LOCATION_UI_ACTION_PANIC_OFF,
} location_ui_action_t;

enum {
    LOCATION_UI_INTERVAL_1_MIN = 60,
    LOCATION_UI_INTERVAL_5_MIN = 300,
    LOCATION_UI_INTERVAL_15_MIN = 900,
    LOCATION_UI_INTERVAL_60_MIN = 3600,
};

typedef struct {
    bool sharing_enabled;
    location_ui_tier_t tier;
    uint16_t interval_s;
    location_ui_source_t source;
    uint32_t last_share_epoch_s;
} location_ui_state_t;

static inline const char *location_ui_tier_label(location_ui_tier_t tier) {
    switch (tier) {
        case LOCATION_UI_TIER_FULL: return "Exact";
        case LOCATION_UI_TIER_PRESENCE: return "Presence";
        case LOCATION_UI_TIER_COARSE:
        default: return "Coarse";
    }
}

static inline const char *location_ui_source_label(location_ui_source_t source) {
    switch (source) {
        case LOCATION_UI_SOURCE_GPS: return "GPS";
        case LOCATION_UI_SOURCE_MANUAL: return "Manual";
        case LOCATION_UI_SOURCE_HYBRID:
        default: return "Hybrid";
    }
}

static inline const char *location_ui_interval_label(uint16_t interval_s) {
    switch (interval_s) {
        case LOCATION_UI_INTERVAL_1_MIN: return "1 min";
        case LOCATION_UI_INTERVAL_15_MIN: return "15 min";
        case LOCATION_UI_INTERVAL_60_MIN: return "60 min";
        case LOCATION_UI_INTERVAL_5_MIN:
        default: return "5 min";
    }
}

static inline void location_ui_apply_action(location_ui_state_t *st,
                                            location_ui_action_t action,
                                            int value) {
    if (!st) return;
    switch (action) {
        case LOCATION_UI_ACTION_SET_SHARING:
            st->sharing_enabled = (value != 0);
            break;
        case LOCATION_UI_ACTION_SET_TIER:
            if (value == LOCATION_UI_TIER_FULL || value == LOCATION_UI_TIER_COARSE || value == LOCATION_UI_TIER_PRESENCE) {
                st->tier = (location_ui_tier_t)value;
            }
            break;
        case LOCATION_UI_ACTION_SET_INTERVAL:
            if (value == LOCATION_UI_INTERVAL_1_MIN ||
                value == LOCATION_UI_INTERVAL_5_MIN ||
                value == LOCATION_UI_INTERVAL_15_MIN ||
                value == LOCATION_UI_INTERVAL_60_MIN) {
                st->interval_s = (uint16_t)value;
            }
            break;
        case LOCATION_UI_ACTION_SET_SOURCE:
            if (value == LOCATION_UI_SOURCE_HYBRID || value == LOCATION_UI_SOURCE_GPS || value == LOCATION_UI_SOURCE_MANUAL) {
                st->source = (location_ui_source_t)value;
            }
            break;
        case LOCATION_UI_ACTION_PANIC_OFF:
            st->sharing_enabled = false;
            break;
        default:
            break;
    }
}

static inline void location_ui_format_last_share(char *out,
                                                 size_t out_len,
                                                 uint32_t last_share_epoch_s,
                                                 uint32_t now_epoch_s) {
    if (!out || out_len == 0) return;
    if (last_share_epoch_s == 0 || now_epoch_s <= last_share_epoch_s) {
        snprintf(out, out_len, "never");
        return;
    }

    uint32_t delta = now_epoch_s - last_share_epoch_s;
    if (delta < 60) {
        snprintf(out, out_len, "%lus ago", (unsigned long)delta);
    } else if (delta < 3600) {
        snprintf(out, out_len, "%lum ago", (unsigned long)(delta / 60));
    } else {
        snprintf(out, out_len, "%luh ago", (unsigned long)(delta / 3600));
    }
}

#endif
