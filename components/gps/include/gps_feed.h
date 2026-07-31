#ifndef BRAMBLE_GPS_FEED_H
#define BRAMBLE_GPS_FEED_H

#include <stddef.h>
#include <stdint.h>
#include "gps.h"         /* gps_fix_cb_t, gps_stats_t */
#include "nmea_parser.h" /* nmea_position_t */

#define GPS_FEED_MAX_LINE 128
#define GPS_FEED_ANTENNA_TTL_MS 60000

/* One instance per driver. No internal locking: single-writer feed calls
 * (bytes/line/reset) and reads from other tasks are the CALLER's locking
 * problem (gps.c today has none and tolerates it; gps_virt wraps a pthread
 * mutex; the nRF driver wraps a FreeRTOS mutex). */
typedef struct {
    char line_buf[GPS_FEED_MAX_LINE];
    int line_pos;
    nmea_position_t acc;    /* running accumulator across sentences */
    bramble_position_t pos; /* last valid fix */
    bool has_fix;
    uint8_t sats_used;
    uint8_t sats_in_view;
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
/* Clears fix/stats/accumulator/banner but keeps cb registration. */
void gps_feed_reset(gps_feed_t* f);

#endif
