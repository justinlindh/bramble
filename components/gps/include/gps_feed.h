#ifndef BRAMBLE_GPS_FEED_H
#define BRAMBLE_GPS_FEED_H

#include <stddef.h>
#include <stdint.h>
#include "gps.h"         /* gps_fix_cb_t, gps_stats_t */
#include "nmea_parser.h" /* nmea_position_t */

#define GPS_FEED_MAX_LINE 128
#define GPS_FEED_ANTENNA_TTL_MS 60000
#define GPS_FEED_GSV_TTL_MS 30000
#define GPS_FEED_MAX_GNSS 6

/* One constellation's GSV state. Talkers are tracked separately because a
 * multi-constellation receiver emits an independent GSV cycle per talker and
 * each carries only its own satellite total. */
typedef struct {
    char talker[3];      /* "" marks a free slot */
    uint8_t in_view;     /* last field-3 total from this talker */
    uint8_t tracked;     /* committed tracked count from the last complete cycle */
    uint8_t snr_max;     /* committed best C/N0 from the last complete cycle */
    uint8_t acc_tracked; /* accumulating across the cycle in flight */
    uint8_t acc_snr_max;
    uint64_t until_ms; /* committed values are stale at or after this time */
} gps_gsv_slot_t;

/* One instance per driver. No internal locking: single-writer feed calls
 * (bytes/line/reset) and reads from other tasks are the CALLER's locking
 * problem (gps.c today has none and tolerates it; gps_virt wraps a pthread
 * mutex; the nRF driver wraps a FreeRTOS mutex). The multi-word members
 * (first_line_ms, the slot until_ms deadlines) can therefore tear on an
 * unlocked reader such as gps.c: those values are only ever compared against
 * a deadline, never accumulated, so the worst outcome is one stale-versus-
 * fresh flip of a single constellation's liveness or of the NMEA age for one
 * UI tick. */
typedef struct {
    char line_buf[GPS_FEED_MAX_LINE];
    int line_pos;
    nmea_position_t acc;    /* running accumulator across sentences */
    bramble_position_t pos; /* last valid fix */
    bool has_fix;
    uint8_t sats_used;
    uint8_t fix_quality;
    gps_gsv_slot_t gsv[GPS_FEED_MAX_GNSS];
    uint64_t first_line_ms;    /* time of the first line since the last reset */
    bool any_line;             /* false until the first line arrives */
    uint64_t antenna_until_ms; /* 0 = no active warning */
    uint8_t utc_hour, utc_min;
    bool utc_valid;
    uint32_t rx_bytes_total;
    uint32_t rx_lines_total;
    char chip_banner[GPS_FEED_MAX_LINE]; /* first $PAIR021* line seen, else "" */
    gps_fix_cb_t cb;
    void* cb_ctx;
} gps_feed_t;

void gps_feed_init(gps_feed_t* f, gps_fix_cb_t cb, void* ctx);
/* Byte-stream input: dollar restart, CR/LF terminate, 128B cap. */
void gps_feed_bytes(gps_feed_t* f, const uint8_t* data, size_t len, uint64_t now_ms);
/* Whole-line input (no CR/LF needed); gps_feed_bytes routes here, and
 * gps_virt calls it directly. Invokes cb on a valid fix. */
void gps_feed_line(gps_feed_t* f, const char* line, uint64_t now_ms);
bool gps_feed_has_fix(const gps_feed_t* f);
bool gps_feed_get_position(const gps_feed_t* f, bramble_position_t* out);
bool gps_feed_get_utc_hm(const gps_feed_t* f, uint8_t* hour, uint8_t* min);
void gps_feed_get_stats(const gps_feed_t* f, uint64_t now_ms, gps_stats_t* out);
/* Clears the current fix and its UTC latch (has_fix, utc_valid) without
 * touching anything else: sats/antenna stats, the GSV slot table, the last
 * GGA fix quality, the NMEA-age origin, rx counters, the chip banner, the
 * accumulator's merged coordinates, and the cb registration all survive.
 * For a driver that needs to invalidate "we have a fix" (e.g. on a power
 * gate re-open) without losing in-progress stats. The invariant that
 * utc_valid implies has_fix (both set together in gps_feed_line) lives
 * entirely inside gps_feed: callers must go through this instead of poking
 * f->has_fix/f->utc_valid directly, or they can desync the two and leak a
 * stale UTC time-of-day past a fix that no longer holds. */
void gps_feed_clear_fix(gps_feed_t* f);
/* Clears fix/stats/accumulator/banner but keeps cb registration. */
void gps_feed_reset(gps_feed_t* f);

#endif
