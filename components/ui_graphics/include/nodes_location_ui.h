#ifndef NODES_LOCATION_UI_H
#define NODES_LOCATION_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static inline const char *nodes_location_action_label(bool has_peer_location) {
    return has_peer_location ? "View" : "Share";
}

static inline void nodes_location_format_status(char *out,
                                                size_t out_len,
                                                bool has_peer_location,
                                                uint32_t received_ms,
                                                uint32_t now_ms) {
    if (!out || out_len == 0) return;

    if (!has_peer_location) {
        snprintf(out, out_len, "Loc unavailable");
        return;
    }

    if (now_ms <= received_ms) {
        snprintf(out, out_len, "Loc just now");
        return;
    }

    uint32_t age_s = (now_ms - received_ms) / 1000U;
    if (age_s < 60U) {
        snprintf(out, out_len, "Loc just now");
    } else if (age_s < 3600U) {
        snprintf(out, out_len, "Loc %lum ago", (unsigned long)(age_s / 60U));
    } else {
        snprintf(out, out_len, "Loc %luh ago", (unsigned long)(age_s / 3600U));
    }
}

#endif
