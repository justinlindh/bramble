#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

int ui_format_msg_line(const ui_msg_line_t* m, char* buf, size_t buf_len) {
    if (!m || !buf || buf_len == 0)
        return 0;

    char sender[10];
    if (m->outgoing) {
        snprintf(sender, sizeof(sender), "me");
    } else if (m->peer_name && m->peer_name[0]) {
        snprintf(sender, sizeof(sender), "%.8s", m->peer_name);
    } else {
        snprintf(sender, sizeof(sender), "%04X", (unsigned)(m->peer_addr & 0xFFFF));
    }

    const char* text = m->text ? m->text : "";
    int text_len = m->text_len;

    bool is_action = (text_len > 9 && text[0] == 0x01 && strncmp(text + 1, "ACTION ", 7) == 0);
    if (is_action) {
        const char* act = text + 8;
        int act_len = text_len - 8;
        if (act_len > 0 && act[act_len - 1] == 0x01)
            act_len--;
        if (act_len < 0)
            act_len = 0;
        return snprintf(buf, buf_len, "* %s %.*s", sender, act_len, act);
    }

    char tag[16] = "";
    if (m->channel_index > 0) {
        if (m->channel_name && m->channel_name[0])
            snprintf(tag, sizeof(tag), "#%.4s ", m->channel_name);
        else
            snprintf(tag, sizeof(tag), "#%d ", m->channel_index);
    }

    /* Suffix: the delivery badge wins; otherwise an age stamp when provided. */
    char suffix[16];
    if (m->badge && m->badge[0]) {
        snprintf(suffix, sizeof(suffix), "%s", m->badge);
    } else if (m->age_s >= 0) {
        if (m->age_s < 60) {
            snprintf(suffix, sizeof(suffix), " %ds", m->age_s);
        } else if (m->age_s < 3600) {
            snprintf(suffix, sizeof(suffix), " %dm", m->age_s / 60);
        } else {
            int hours = m->age_s / 3600;
            if (hours > 99)
                hours = 99; /* display cap; the store never holds days anyway */
            snprintf(suffix, sizeof(suffix), " %dh", hours);
        }
    } else {
        suffix[0] = '\0';
    }

    int used = (int)strlen(tag) + (int)strlen(sender) + 2 + (int)strlen(suffix);
    int text_max = (int)buf_len - 1 - used;
    if (text_max < 1)
        text_max = 1;
    if (text_len > text_max)
        text_len = text_max;
    return snprintf(buf, buf_len, "%s%s: %.*s%s", tag, sender, text_len, text, suffix);
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
