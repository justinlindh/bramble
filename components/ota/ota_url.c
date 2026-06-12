#include "ota_url.h"

#include <ctype.h>
#include <string.h>

static bool has_prefix_nocase(const char* s, const char* prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

/* Charset allowed in an origin AFTER the scheme: host, port, path. */
static bool origin_char_ok(char c) {
    if (isalnum((unsigned char)c)) {
        return true;
    }
    return c == '.' || c == '-' || c == '_' || c == '~' || c == ':' || c == '/';
}

bool ota_url_origin_valid(const char* origin, bool allow_http) {
    if (!origin) {
        return false;
    }
    size_t len = strlen(origin);
    if (len == 0 || len >= OTA_URL_MAX) {
        return false;
    }

    const char* rest;
    if (has_prefix_nocase(origin, "https://")) {
        rest = origin + 8;
    } else if (allow_http && has_prefix_nocase(origin, "http://")) {
        rest = origin + 7;
    } else {
        return false;
    }

    /* Host must be non-empty: at least one allowed char before any '/'. */
    if (*rest == '\0' || *rest == '/' || *rest == ':') {
        return false;
    }

    for (const char* p = rest; *p; p++) {
        if (!origin_char_ok(*p)) {
            return false;
        }
    }
    return true;
}

bool ota_url_path_valid(const char* path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    if (strlen(path) >= OTA_URL_MAX) {
        return false;
    }
    if (path[0] == '/' || path[strlen(path) - 1] == '/') {
        return false;
    }

    /* Charset allowlist: kills ':', '%', '?', '#', '@', '\\', whitespace and
     * control characters in one pass. */
    for (const char* p = path; *p; p++) {
        char c = *p;
        if (isalnum((unsigned char)c)) {
            continue;
        }
        if (c == '.' || c == '_' || c == '+' || c == '-' || c == '/') {
            continue;
        }
        return false;
    }

    /* Segment checks: no empty segments, no "." or ".." segments. */
    const char* seg = path;
    while (1) {
        const char* slash = strchr(seg, '/');
        size_t seg_len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (seg_len == 0) {
            return false; /* "//" */
        }
        if ((seg_len == 1 && seg[0] == '.') || (seg_len == 2 && seg[0] == '.' && seg[1] == '.')) {
            return false;
        }
        if (!slash) {
            break;
        }
        seg = slash + 1;
    }
    return true;
}

int ota_url_resolve(const char* origin, const char* path, bool allow_http, char* out,
                    size_t out_len) {
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    if (!out || out_len == 0) {
        return OTA_URL_ERR_TOOLONG;
    }
    if (!ota_url_origin_valid(origin, allow_http)) {
        return OTA_URL_ERR_ORIGIN;
    }
    if (!ota_url_path_valid(path)) {
        return OTA_URL_ERR_PATH;
    }

    size_t origin_len = strlen(origin);
    bool need_slash = origin[origin_len - 1] != '/';
    size_t total = origin_len + (need_slash ? 1 : 0) + strlen(path);
    if (total >= out_len || total >= OTA_URL_MAX) {
        out[0] = '\0';
        return OTA_URL_ERR_TOOLONG;
    }

    memcpy(out, origin, origin_len);
    size_t pos = origin_len;
    if (need_slash) {
        out[pos++] = '/';
    }
    strcpy(out + pos, path); /* bounds checked above */
    return OTA_URL_OK;
}
