#ifndef NODE_DETAIL_UI_H
#define NODE_DETAIL_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static inline void node_detail_format_last_seen(char* out, size_t out_len, uint32_t last_seen_ms,
                                                uint32_t now_ms) {
    if (!out || out_len == 0)
        return;

    if (now_ms <= last_seen_ms) {
        snprintf(out, out_len, "Last seen now");
        return;
    }

    uint32_t age_s = (now_ms - last_seen_ms) / 1000U;
    if (age_s < 60U) {
        snprintf(out, out_len, "Last seen %lus ago", (unsigned long)age_s);
    } else if (age_s < 3600U) {
        snprintf(out, out_len, "Last seen %lum ago", (unsigned long)(age_s / 60U));
    } else {
        snprintf(out, out_len, "Last seen %luh ago", (unsigned long)(age_s / 3600U));
    }
}

static inline void node_detail_format_location(char* out, size_t out_len, bool has_location,
                                               int32_t latitude_e7, int32_t longitude_e7,
                                               uint32_t received_ms, uint32_t now_ms) {
    if (!out || out_len == 0)
        return;

    if (!has_location) {
        snprintf(out, out_len, "No location shared");
        return;
    }

    double lat = ((double)latitude_e7) / 1e7;
    double lon = ((double)longitude_e7) / 1e7;
    uint32_t age_s = (now_ms > received_ms) ? (now_ms - received_ms) / 1000U : 0U;
    if (age_s < 60U) {
        snprintf(out, out_len, "%.6f, %.6f (now)", lat, lon);
    } else if (age_s < 3600U) {
        snprintf(out, out_len, "%.6f, %.6f (%lum ago)", lat, lon, (unsigned long)(age_s / 60U));
    } else {
        snprintf(out, out_len, "%.6f, %.6f (%luh ago)", lat, lon, (unsigned long)(age_s / 3600U));
    }
}

#endif
