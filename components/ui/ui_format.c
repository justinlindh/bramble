#include "ui.h"
#include <stdio.h>
#include <string.h>

int ui_format_main_line1(const ui_main_data_t *data, char *buf, size_t buf_len) {
    return snprintf(buf, buf_len, "%08X  %u%%", data->my_addr, data->battery_pct);
}

int ui_format_main_line2(const ui_main_data_t *data, char *buf, size_t buf_len) {
    return snprintf(buf, buf_len, "%u peers  TX:%u", data->neighbor_count, data->tx_queue_depth);
}

int ui_format_main_line3(const ui_main_data_t *data, char *buf, size_t buf_len) {
    char uptime[32];
    ui_format_uptime(data->uptime_sec, uptime, sizeof(uptime));
    return snprintf(buf, buf_len, "Up: %s", uptime);
}

int ui_format_uptime(uint32_t uptime_sec, char *buf, size_t buf_len) {
    if (uptime_sec < 60) {
        return snprintf(buf, buf_len, "%us", uptime_sec);
    } else if (uptime_sec < 3600) {
        uint32_t m = uptime_sec / 60;
        uint32_t s = uptime_sec % 60;
        return snprintf(buf, buf_len, "%um %us", m, s);
    } else if (uptime_sec < 86400) {
        uint32_t h = uptime_sec / 3600;
        uint32_t m = (uptime_sec % 3600) / 60;
        return snprintf(buf, buf_len, "%uh %um", h, m);
    } else {
        uint32_t d = uptime_sec / 86400;
        uint32_t h = (uptime_sec % 86400) / 3600;
        return snprintf(buf, buf_len, "%ud %uh", d, h);
    }
}
