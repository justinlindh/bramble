#ifndef BRAMBLE_TZ_H
#define BRAMBLE_TZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Local-time conversion from a POSIX TZ specification.
 *
 * Bramble nodes have no internet, so there is no tzdata to ship or refresh
 * and no NTP to lean on. A POSIX TZ string carries the whole zone definition
 * inline, including the daylight-saving transition rules, in a few dozen
 * bytes: "PST8PDT,M3.2.0,M11.1.0" is a complete, self-contained description
 * of US Pacific that stays correct across transitions without any external
 * data source.
 *
 * The conversion here is a pure function of (UTC wall clock, TZ spec). It
 * does not read or write the process-global TZ environment, so it is safe to
 * call from a render task and behaves identically on the host and on device.
 *
 * Supported grammar (POSIX.1 TZ, the M/J/n rule forms):
 *   std offset [dst [offset] [,start[/time],end[/time]]]
 * where a name is three or more alphabetic characters or an angle-bracket
 * quoted form such as <+0545>, an offset is [+|-]hh[:mm[:ss]] measured
 * WEST-positive (UTC = local + offset, so US Pacific standard time is 8), and
 * a rule is Mm.w.d, Jn, or n. Transition times default to 02:00:00 local.
 */

/* Longest accepted spec including the terminating NUL. The longest realistic
 * spec ("<+0545>-5:45<+0545>,M3.2.0/2:00:00,M11.1.0/2:00:00" style) fits. */
#define BRAMBLE_TZ_SPEC_MAX 64

/* Longest zone abbreviation including the terminating NUL. POSIX caps a
 * quoted name at six characters. */
#define BRAMBLE_TZ_ABBREV_MAX 8

/* A broken-down wall clock at minute resolution, which is all the status bar
 * renders and all any Bramble time source supplies. */
typedef struct {
    int32_t year;   /* full proleptic Gregorian year, e.g. 2026 */
    uint8_t month;  /* 1-12 */
    uint8_t day;    /* 1-31 */
    uint8_t hour;   /* 0-23 */
    uint8_t minute; /* 0-59 */
} bramble_tz_time_t;

typedef enum {
    BRAMBLE_TZ_STD = 0,  /* converted; standard time in effect */
    BRAMBLE_TZ_DST,      /* converted; daylight saving in effect */
    BRAMBLE_TZ_BAD_SPEC, /* spec absent or unparseable; no output written */
    BRAMBLE_TZ_BAD_TIME, /* UTC input is not a valid date; no output written */
} bramble_tz_status_t;

/* True when spec parses as a complete POSIX TZ specification. An empty or
 * NULL spec is not valid: callers store BRAMBLE_TZ_DEFAULT_SPEC instead of
 * leaving the zone unset, so that a displayed clock always names a zone. */
bool bramble_tz_spec_valid(const char* spec);

/*
 * Convert a UTC wall clock to local time under spec.
 *
 * out_local and out_abbrev are written only on BRAMBLE_TZ_STD/BRAMBLE_TZ_DST.
 * Both outputs are optional. out_abbrev receives the zone abbreviation in
 * effect ("PST" or "PDT" for US Pacific) and is always NUL-terminated when
 * abbrev_len is non-zero.
 */
bramble_tz_status_t bramble_tz_localtime(const char* spec, const bramble_tz_time_t* utc,
                                         bramble_tz_time_t* out_local, char* out_abbrev,
                                         size_t abbrev_len);

/* The zone a node uses until one is chosen: plain UTC with no DST rule. A
 * node that has never been configured therefore shows real UTC rather than a
 * guessed local time. */
#define BRAMBLE_TZ_DEFAULT_SPEC "UTC0"

/* A named zone offered by the on-device Settings picker and by clients. The
 * table is the single source of truth for both, so a client never has to
 * carry its own copy of the specs. */
typedef struct {
    const char* label; /* human-facing name, e.g. "US Pacific" */
    const char* spec;  /* POSIX TZ spec the label maps to */
} bramble_tz_preset_t;

size_t bramble_tz_preset_count(void);

/* Preset at index, or NULL when index is out of range. */
const bramble_tz_preset_t* bramble_tz_preset(size_t index);

#endif /* BRAMBLE_TZ_H */
