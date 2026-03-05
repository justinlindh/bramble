#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

int ui_format_main_line1(const ui_main_data_t* data, char* buf, size_t buf_len) {
    return snprintf(buf, buf_len, "%08" PRIX32 "  %u%%", data->my_addr, data->battery_pct);
}

int ui_format_main_line2(const ui_main_data_t* data, char* buf, size_t buf_len) {
    return snprintf(buf, buf_len, "%u peers  TX:%u", data->neighbor_count, data->tx_queue_depth);
}

int ui_format_main_line3(const ui_main_data_t* data, char* buf, size_t buf_len) {
    char uptime[32];
    ui_format_uptime(data->uptime_sec, uptime, sizeof(uptime));
    return snprintf(buf, buf_len, "Up: %s", uptime);
}

int ui_format_uptime(uint32_t uptime_sec, char* buf, size_t buf_len) {
    if (uptime_sec < 60) {
        return snprintf(buf, buf_len, "%" PRIu32 "s", uptime_sec);
    } else if (uptime_sec < 3600) {
        uint32_t m = uptime_sec / 60;
        uint32_t s = uptime_sec % 60;
        return snprintf(buf, buf_len, "%" PRIu32 "m %" PRIu32 "s", m, s);
    } else if (uptime_sec < 86400) {
        uint32_t h = uptime_sec / 3600;
        uint32_t m = (uptime_sec % 3600) / 60;
        return snprintf(buf, buf_len, "%" PRIu32 "h %" PRIu32 "m", h, m);
    } else {
        uint32_t d = uptime_sec / 86400;
        uint32_t h = (uptime_sec % 86400) / 3600;
        return snprintf(buf, buf_len, "%" PRIu32 "d %" PRIu32 "h", d, h);
    }
}
