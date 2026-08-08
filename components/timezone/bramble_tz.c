#include "bramble_tz.h"

#include <string.h>

/* Rule forms POSIX allows for a transition date. */
typedef enum {
    RULE_NONE = 0,
    RULE_MWD, /* Mm.w.d: month, week-of-month, weekday */
    RULE_J,   /* Jn: 1-365, February 29 never counted */
    RULE_N,   /* n: 0-365, February 29 counted */
} rule_kind_t;

typedef struct {
    rule_kind_t kind;
    uint8_t month; /* RULE_MWD: 1-12 */
    uint8_t week;  /* RULE_MWD: 1-5, where 5 means "last" */
    uint8_t dow;   /* RULE_MWD: 0-6, 0 = Sunday */
    uint16_t doy;  /* RULE_J: 1-365. RULE_N: 0-365 */
    int32_t time_s;
} tz_rule_t;

typedef struct {
    char std_abbr[BRAMBLE_TZ_ABBREV_MAX];
    char dst_abbr[BRAMBLE_TZ_ABBREV_MAX];
    int32_t std_off_s; /* west-positive: UTC = local + off */
    int32_t dst_off_s;
    bool has_dst;
    tz_rule_t start;
    tz_rule_t end;
} tz_spec_t;

/* ── Civil-date arithmetic ─────────────────────────────────────────────────
 * days_from_civil/civil_from_days are the standard proleptic-Gregorian pair
 * (Howard Hinnant's chrono algorithms), valid across the whole range a node
 * can produce. Days are counted from 1970-01-01. */

static int64_t days_from_civil(int32_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - (int32_t)(era * 400));
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int32_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = (int32_t)(yy + (mm <= 2u));
    *m = mm;
    *d = dd;
}

static bool is_leap(int32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static unsigned days_in_month(int32_t y, unsigned m) {
    static const unsigned k[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (m < 1u || m > 12u)
        return 0u;
    if (m == 2u && is_leap(y))
        return 29u;
    return k[m - 1u];
}

/* 0 = Sunday. 1970-01-01 was a Thursday, hence the +4. */
static unsigned weekday_from_days(int64_t days) {
    int64_t w = (days + 4) % 7;
    if (w < 0)
        w += 7;
    return (unsigned)w;
}

/* ── Spec parsing ───────────────────────────────────────────────────────── */

static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Parse a zone name: three or more alphabetic characters, or an
 * angle-bracket quoted run of alphanumerics and sign characters. */
static bool parse_abbr(const char** p, char* out, size_t out_len) {
    const char* s = *p;
    size_t n = 0;

    if (*s == '<') {
        s++;
        while (*s && *s != '>') {
            if (!is_alpha(*s) && !is_digit(*s) && *s != '+' && *s != '-')
                return false;
            if (n + 1 < out_len)
                out[n] = *s;
            n++;
            s++;
        }
        if (*s != '>')
            return false;
        s++;
    } else {
        while (is_alpha(*s)) {
            if (n + 1 < out_len)
                out[n] = *s;
            n++;
            s++;
        }
    }

    /* POSIX requires at least three characters, and a quoted name is capped
     * at six. Anything outside that is a malformed spec rather than a name we
     * silently truncate. */
    if (n < 3 || n > BRAMBLE_TZ_ABBREV_MAX - 1)
        return false;

    out[n] = '\0';
    *p = s;
    return true;
}

/* Parse [+|-]hh[:mm[:ss]] into seconds. hh_max bounds the hour field: POSIX
 * allows 0-24 for a zone offset and 0-167 for a transition time. */
static bool parse_hms(const char** p, int32_t* out_s, int hh_max) {
    const char* s = *p;
    int sign = 1;

    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        sign = -1;
        s++;
    }

    if (!is_digit(*s))
        return false;

    int32_t hh = 0;
    int digits = 0;
    while (is_digit(*s) && digits < 3) {
        hh = hh * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (hh > hh_max)
        return false;

    int32_t mm = 0;
    int32_t ss = 0;
    if (*s == ':') {
        s++;
        if (!is_digit(*s) || !is_digit(s[1]))
            return false;
        mm = (s[0] - '0') * 10 + (s[1] - '0');
        s += 2;
        if (mm > 59)
            return false;
        if (*s == ':') {
            s++;
            if (!is_digit(*s) || !is_digit(s[1]))
                return false;
            ss = (s[0] - '0') * 10 + (s[1] - '0');
            s += 2;
            if (ss > 59)
                return false;
        }
    }

    *out_s = sign * (hh * 3600 + mm * 60 + ss);
    *p = s;
    return true;
}

static bool parse_rule(const char** p, tz_rule_t* out) {
    const char* s = *p;

    memset(out, 0, sizeof(*out));

    if (*s == 'M') {
        s++;
        int32_t m = 0, w = 0, d = 0;
        if (!is_digit(*s))
            return false;
        while (is_digit(*s))
            m = m * 10 + (*s++ - '0');
        if (*s++ != '.')
            return false;
        if (!is_digit(*s))
            return false;
        while (is_digit(*s))
            w = w * 10 + (*s++ - '0');
        if (*s++ != '.')
            return false;
        if (!is_digit(*s))
            return false;
        while (is_digit(*s))
            d = d * 10 + (*s++ - '0');
        if (m < 1 || m > 12 || w < 1 || w > 5 || d < 0 || d > 6)
            return false;
        out->kind = RULE_MWD;
        out->month = (uint8_t)m;
        out->week = (uint8_t)w;
        out->dow = (uint8_t)d;
    } else if (*s == 'J') {
        s++;
        int32_t n = 0;
        if (!is_digit(*s))
            return false;
        while (is_digit(*s))
            n = n * 10 + (*s++ - '0');
        if (n < 1 || n > 365)
            return false;
        out->kind = RULE_J;
        out->doy = (uint16_t)n;
    } else if (is_digit(*s)) {
        int32_t n = 0;
        while (is_digit(*s))
            n = n * 10 + (*s++ - '0');
        if (n > 365)
            return false;
        out->kind = RULE_N;
        out->doy = (uint16_t)n;
    } else {
        return false;
    }

    /* Transition time defaults to 02:00:00 local when no /time is given. */
    out->time_s = 2 * 3600;
    if (*s == '/') {
        s++;
        if (!parse_hms(&s, &out->time_s, 167))
            return false;
    }

    *p = s;
    return true;
}

static bool parse_spec(const char* spec, tz_spec_t* out) {
    if (!spec || spec[0] == '\0')
        return false;
    if (strlen(spec) >= BRAMBLE_TZ_SPEC_MAX)
        return false;

    memset(out, 0, sizeof(*out));

    const char* p = spec;

    if (!parse_abbr(&p, out->std_abbr, sizeof(out->std_abbr)))
        return false;
    if (!parse_hms(&p, &out->std_off_s, 24))
        return false;

    if (*p == '\0') {
        /* Standard time only, no daylight saving. */
        return true;
    }

    if (!parse_abbr(&p, out->dst_abbr, sizeof(out->dst_abbr)))
        return false;
    out->has_dst = true;

    /* An explicit DST offset is optional; it defaults to one hour east of
     * standard time. */
    if (*p != ',' && *p != '\0') {
        if (!parse_hms(&p, &out->dst_off_s, 24))
            return false;
    } else {
        out->dst_off_s = out->std_off_s - 3600;
    }

    /* Bramble requires the transition rules to be spelled out. POSIX leaves
     * the ruleless form implementation-defined, and guessing US rules for a
     * spec that never stated them would silently produce a wrong clock in
     * every zone that does not follow them. */
    if (*p != ',')
        return false;
    p++;
    if (!parse_rule(&p, &out->start))
        return false;
    if (*p != ',')
        return false;
    p++;
    if (!parse_rule(&p, &out->end))
        return false;

    return *p == '\0';
}

/* ── Transition instants ────────────────────────────────────────────────── */

/* Days since the epoch for the civil date a rule selects within year. */
static int64_t rule_days(const tz_rule_t* r, int32_t year) {
    if (r->kind == RULE_MWD) {
        const int64_t first = days_from_civil(year, r->month, 1u);
        const unsigned first_dow = weekday_from_days(first);
        unsigned dom = 1u + ((r->dow + 7u - first_dow) % 7u) + 7u * (r->week - 1u);
        const unsigned dim = days_in_month(year, r->month);
        /* Week 5 means "the last such weekday", and an over-long week for a
         * short month falls back the same way. */
        while (dom > dim)
            dom -= 7u;
        return days_from_civil(year, r->month, dom);
    }

    if (r->kind == RULE_J) {
        /* Jn never counts February 29, so it names the same month and day
         * every year: resolve it against a known non-leap year. */
        const int64_t base = days_from_civil(1970, 1u, 1u) + (int64_t)r->doy - 1;
        int32_t by;
        unsigned bm, bd;
        civil_from_days(base, &by, &bm, &bd);
        return days_from_civil(year, bm, bd);
    }

    /* RULE_N counts from January 1 as day 0 and does count February 29. */
    return days_from_civil(year, 1u, 1u) + (int64_t)r->doy;
}

/* Seconds since the epoch, in UTC, at which a rule fires in year. The rule's
 * time is local, expressed in whichever offset is in force on that side of
 * the transition. */
static int64_t rule_instant_utc(const tz_rule_t* r, int32_t year, int32_t off_s) {
    return rule_days(r, year) * 86400 + (int64_t)r->time_s + (int64_t)off_s;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool bramble_tz_spec_valid(const char* spec) {
    tz_spec_t parsed;
    return parse_spec(spec, &parsed);
}

bramble_tz_status_t bramble_tz_localtime(const char* spec, const bramble_tz_time_t* utc,
                                         bramble_tz_time_t* out_local, char* out_abbrev,
                                         size_t abbrev_len) {
    if (out_abbrev && abbrev_len > 0)
        out_abbrev[0] = '\0';

    tz_spec_t tz;
    if (!parse_spec(spec, &tz))
        return BRAMBLE_TZ_BAD_SPEC;

    if (!utc)
        return BRAMBLE_TZ_BAD_TIME;
    if (utc->month < 1u || utc->month > 12u || utc->day < 1u ||
        utc->day > days_in_month(utc->year, utc->month) || utc->hour > 23u || utc->minute > 59u) {
        return BRAMBLE_TZ_BAD_TIME;
    }

    const int64_t t = days_from_civil(utc->year, utc->month, utc->day) * 86400 + utc->hour * 3600 +
                      utc->minute * 60;

    bool dst = false;
    if (tz.has_dst) {
        /* Both transitions are evaluated in the UTC year. For a
         * northern-hemisphere zone DST is the interval between them; for a
         * southern-hemisphere zone the interval wraps the year boundary, which
         * is exactly the case start > end distinguishes. */
        const int64_t start = rule_instant_utc(&tz.start, utc->year, tz.std_off_s);
        const int64_t end = rule_instant_utc(&tz.end, utc->year, tz.dst_off_s);
        dst = (start <= end) ? (t >= start && t < end) : (t >= start || t < end);
    }

    const int32_t off = dst ? tz.dst_off_s : tz.std_off_s;
    const int64_t local = t - off;

    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    if (out_local) {
        int32_t y;
        unsigned m, d;
        civil_from_days(days, &y, &m, &d);
        out_local->year = y;
        out_local->month = (uint8_t)m;
        out_local->day = (uint8_t)d;
        out_local->hour = (uint8_t)(rem / 3600);
        out_local->minute = (uint8_t)((rem % 3600) / 60);
    }

    if (out_abbrev && abbrev_len > 0) {
        const char* src = dst ? tz.dst_abbr : tz.std_abbr;
        size_t n = strlen(src);
        if (n >= abbrev_len)
            n = abbrev_len - 1;
        memcpy(out_abbrev, src, n);
        out_abbrev[n] = '\0';
    }

    return dst ? BRAMBLE_TZ_DST : BRAMBLE_TZ_STD;
}

/* Zones offered by the on-device picker. Deliberately a short list of common
 * ones rather than a tzdata port: a node has no way to refresh a database, and
 * anything absent here is still reachable by sending a POSIX spec over RPC.
 * Rules current as of 2026-08. */
static const bramble_tz_preset_t k_presets[] = {
    {"UTC", "UTC0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Arizona", "MST7"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Hawaii", "HST10"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"India", "IST-5:30"},
    {"China", "CST-8"},
    {"Japan", "JST-9"},
    {"Australia Eastern", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

size_t bramble_tz_preset_count(void) { return sizeof(k_presets) / sizeof(k_presets[0]); }

const bramble_tz_preset_t* bramble_tz_preset(size_t index) {
    if (index >= bramble_tz_preset_count())
        return NULL;
    return &k_presets[index];
}
