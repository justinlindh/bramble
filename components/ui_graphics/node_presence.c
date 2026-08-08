#include "node_presence.h"

#include <stdio.h>

uint32_t node_age_seconds(uint32_t now_ms, uint32_t last_heard_ms) {
    if (now_ms <= last_heard_ms)
        return 0;
    return (now_ms - last_heard_ms) / 1000U;
}

node_presence_t node_presence_for_age(uint32_t age_s) {
    return (age_s >= NODE_STALE_AGE_S) ? NODE_PRESENCE_STALE : NODE_PRESENCE_LIVE;
}

node_reach_t node_reach_classify(bool has_neighbor, uint32_t age_s, bool has_active_route) {
    if (has_neighbor) {
        return (age_s < NODE_ONLINE_AGE_S) ? NODE_REACH_ONLINE : NODE_REACH_REACHABLE;
    }
    return has_active_route ? NODE_REACH_REACHABLE : NODE_REACH_UNKNOWN;
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
