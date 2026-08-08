#include "node_presence.h"

#include <stdio.h>

uint32_t node_age_seconds(uint32_t now_ms, uint32_t last_heard_ms) {
    if (now_ms <= last_heard_ms)
        return 0;
    return (now_ms - last_heard_ms) / 1000U;
}

bool node_is_stale(uint32_t age_s) { return age_s >= NODE_STALE_AGE_S; }

int node_signal_pct(int8_t rssi) {
    int pct = ((int)rssi + 120) * 100 / 70;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

int node_format_age(uint32_t age_s, char* buf, size_t buf_len) {
    if (!buf || buf_len == 0)
        return 0;
    if (age_s < 60U)
        return snprintf(buf, buf_len, "%us", (unsigned)age_s);
    if (age_s < 3600U)
        return snprintf(buf, buf_len, "%um %us", (unsigned)(age_s / 60U), (unsigned)(age_s % 60U));
    if (age_s < 86400U)
        return snprintf(buf, buf_len, "%uh %um", (unsigned)(age_s / 3600U),
                        (unsigned)((age_s % 3600U) / 60U));
    return snprintf(buf, buf_len, "%ud %uh", (unsigned)(age_s / 86400U),
                    (unsigned)((age_s % 86400U) / 3600U));
}
