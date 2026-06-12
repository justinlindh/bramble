#include "ota_version.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Parse a non-negative decimal integer with no sign and no leading garbage.
 * Advances *s past the digits. Returns false if no digits present. */
static bool parse_uint(const char** s, int* out) {
    const char* p = *s;
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        if (v > 1000000) {
            return false;
        }
        p++;
    }
    *s = p;
    *out = (int)v;
    return true;
}

static bool prerelease_char_ok(char c) { return isalnum((unsigned char)c) || c == '.' || c == '-'; }

bool ota_version_parse(const char* s, ota_semver_t* out) {
    if (!s || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (*s == 'v' || *s == 'V') {
        s++;
    }

    if (!parse_uint(&s, &out->major) || *s++ != '.') {
        return false;
    }
    if (!parse_uint(&s, &out->minor) || *s++ != '.') {
        return false;
    }
    if (!parse_uint(&s, &out->patch)) {
        return false;
    }

    if (*s == '-') {
        s++;
        const char* start = s;
        while (*s && *s != '+') {
            if (!prerelease_char_ok(*s)) {
                return false;
            }
            s++;
        }
        size_t len = (size_t)(s - start);
        if (len == 0 || len >= OTA_VERSION_PRERELEASE_MAX) {
            return false;
        }
        /* No empty identifiers: leading/trailing/double dots. */
        if (start[0] == '.' || start[len - 1] == '.' || strstr(start, "..") != NULL) {
            return false;
        }
        memcpy(out->prerelease, start, len);
        out->prerelease[len] = '\0';
    }

    /* Build metadata is ignored; anything else is malformed. */
    if (*s == '+') {
        return true;
    }
    return *s == '\0';
}

/* Compare one dot-separated prerelease identifier per semver:
 * numeric < alphanumeric; numerics compare numerically; others by ASCII. */
static int cmp_identifier(const char* a, size_t alen, const char* b, size_t blen) {
    bool a_num = true;
    bool b_num = true;
    for (size_t i = 0; i < alen; i++) {
        if (!isdigit((unsigned char)a[i])) {
            a_num = false;
            break;
        }
    }
    for (size_t i = 0; i < blen; i++) {
        if (!isdigit((unsigned char)b[i])) {
            b_num = false;
            break;
        }
    }
    if (a_num && b_num) {
        /* Numeric: longer string of digits is larger; equal length is memcmp. */
        if (alen != blen) {
            return alen < blen ? -1 : 1;
        }
        int c = memcmp(a, b, alen);
        return (c > 0) - (c < 0);
    }
    if (a_num != b_num) {
        return a_num ? -1 : 1;
    }
    size_t min = alen < blen ? alen : blen;
    int c = memcmp(a, b, min);
    if (c != 0) {
        return (c > 0) - (c < 0);
    }
    if (alen != blen) {
        return alen < blen ? -1 : 1;
    }
    return 0;
}

static int cmp_prerelease(const char* a, const char* b) {
    while (*a || *b) {
        /* Fewer fields ranks lower when all preceding fields are equal. */
        if (*a == '\0') {
            return -1;
        }
        if (*b == '\0') {
            return 1;
        }
        const char* a_end = strchr(a, '.');
        const char* b_end = strchr(b, '.');
        size_t alen = a_end ? (size_t)(a_end - a) : strlen(a);
        size_t blen = b_end ? (size_t)(b_end - b) : strlen(b);
        int c = cmp_identifier(a, alen, b, blen);
        if (c != 0) {
            return c;
        }
        a += alen + (a_end ? 1 : 0);
        b += blen + (b_end ? 1 : 0);
    }
    return 0;
}

int ota_version_cmp(const ota_semver_t* a, const ota_semver_t* b) {
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    bool a_pre = a->prerelease[0] != '\0';
    bool b_pre = b->prerelease[0] != '\0';
    if (a_pre != b_pre) {
        return a_pre ? -1 : 1; /* release outranks prerelease */
    }
    if (!a_pre) {
        return 0;
    }
    return cmp_prerelease(a->prerelease, b->prerelease);
}

bool ota_version_cmp_str(const char* a, const char* b, int* cmp) {
    ota_semver_t va;
    ota_semver_t vb;
    if (!ota_version_parse(a, &va) || !ota_version_parse(b, &vb)) {
        return false;
    }
    *cmp = ota_version_cmp(&va, &vb);
    return true;
}
