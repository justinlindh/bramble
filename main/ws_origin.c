#include "ws_origin.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static int ieq_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

/* Extract the host portion of "host[:port]" or "scheme://host[:port][/...]".
 * Returns pointer to host start and writes its length; NULL if empty. */
static const char* host_part(const char* s, size_t* len_out) {
    if (!s || s[0] == '\0') {
        return NULL;
    }
    const char* p = strstr(s, "://");
    const char* host = p ? p + 3 : s;
    size_t len = 0;
    /* IPv6 literal: [::1]:port */
    if (host[0] == '[') {
        const char* close = strchr(host, ']');
        if (!close) {
            return NULL;
        }
        len = (size_t)(close - host) + 1;
    } else {
        while (host[len] != '\0' && host[len] != ':' && host[len] != '/') {
            len++;
        }
    }
    if (len == 0) {
        return NULL;
    }
    *len_out = len;
    return host;
}

/* Compare a full origin against one allowlist entry, case-insensitive,
 * ignoring one trailing '/' on either side. */
static bool origin_entry_matches(const char* origin, const char* entry, size_t entry_len) {
    size_t olen = strlen(origin);
    if (olen > 0 && origin[olen - 1] == '/') {
        olen--;
    }
    if (entry_len > 0 && entry[entry_len - 1] == '/') {
        entry_len--;
    }
    return olen == entry_len && olen > 0 && ieq_n(origin, entry, olen);
}

bool ws_origin_allowed(const char* origin, const char* host_hdr, const char* extra_origins) {
    /* No Origin header: not a browser, not CSWSH-able. */
    if (!origin || origin[0] == '\0') {
        return true;
    }

    /* Same-origin: Origin host == Host header host, any port/scheme.
     * "null" and malformed origins have no "://" and fall through to the
     * extras check. */
    if (strstr(origin, "://") != NULL) {
        size_t ohlen = 0;
        size_t hhlen = 0;
        const char* ohost = host_part(origin, &ohlen);
        const char* hhost = host_part(host_hdr, &hhlen);
        if (ohost && hhost && ohlen == hhlen && ieq_n(ohost, hhost, ohlen)) {
            return true;
        }
    }

    /* Configured extras: comma-separated full origins. */
    if (extra_origins) {
        const char* p = extra_origins;
        while (*p != '\0') {
            while (*p == ',' || *p == ' ') {
                p++;
            }
            size_t len = 0;
            while (p[len] != '\0' && p[len] != ',' && p[len] != ' ') {
                len++;
            }
            if (len > 0 && origin_entry_matches(origin, p, len)) {
                return true;
            }
            p += len;
        }
    }

    return false;
}
