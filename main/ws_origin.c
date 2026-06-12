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
 * Returns pointer to host start and writes its length; NULL if empty or
 * malformed. A single trailing dot is stripped (FQDN equivalence:
 * "device.local." is "device.local"). Any '@' in the authority is treated
 * as malformed: Origin serialization never carries userinfo, and parsing
 * around it risks "http://192.168.4.1@evil.com" confusions; rejecting is
 * the fail-safe answer. */
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
            if (host[len] == '@') {
                return NULL;
            }
            len++;
        }
    }
    /* FQDN trailing-dot equivalence */
    if (len > 1 && host[len - 1] == '.') {
        len--;
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

/* Same-origin test shared by the Origin and Referer paths. */
static bool same_origin_host(const char* url, const char* host_hdr) {
    size_t ulen = 0;
    size_t hlen = 0;
    const char* uhost = host_part(url, &ulen);
    const char* hhost = host_part(host_hdr, &hlen);
    return uhost && hhost && ulen == hlen && ieq_n(uhost, hhost, ulen);
}

bool ws_config_post_allowed(const char* origin, const char* referer, const char* host_hdr,
                            const char* extra_origins) {
    if (origin && origin[0] != '\0') {
        return ws_origin_allowed(origin, host_hdr, extra_origins);
    }
    if (referer && referer[0] != '\0') {
        /* Referer is a full URL with a path; only the same-origin test
           is meaningful (exact-match extras cannot match it). It must
           contain a scheme to be parseable at all. */
        if (strstr(referer, "://") == NULL) {
            return false;
        }
        return same_origin_host(referer, host_hdr);
    }
    return true; /* no browser-provenance headers: not CSRF-able */
}

bool ws_origin_allowed(const char* origin, const char* host_hdr, const char* extra_origins) {
    /* No Origin header: not a browser, not CSWSH-able. */
    if (!origin || origin[0] == '\0') {
        return true;
    }

    /* Same-origin: Origin host == Host header host, any port/scheme.
     * "null" and malformed origins have no "://" and fall through to the
     * extras check. */
    if (strstr(origin, "://") != NULL && same_origin_host(origin, host_hdr)) {
        return true;
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
