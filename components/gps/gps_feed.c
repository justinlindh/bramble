#include "gps_feed.h"
#include "nmea_parser.h"
#include <string.h>

void gps_feed_init(gps_feed_t* f, gps_fix_cb_t cb, void* ctx) {
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->cb_ctx = ctx;
}

void gps_feed_clear_fix(gps_feed_t* f) {
    f->has_fix = false;
    f->utc_valid = false;
}

void gps_feed_reset(gps_feed_t* f) {
    gps_fix_cb_t cb = f->cb;
    void* cb_ctx = f->cb_ctx;
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->cb_ctx = cb_ctx;
}

void gps_feed_line(gps_feed_t* f, const char* line, uint64_t now_ms) {
    bool parsed = false;

    if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        char copy[GPS_FEED_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        parsed = nmea_parse_rmc(copy, &f->acc);
    } else if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
        char copy[GPS_FEED_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        parsed = nmea_parse_gga(copy, &f->acc);
        /* Satellites-used is reported even without a fix. */
        f->sats_used = f->acc.sats_used;
    } else if (strncmp(line, "$GPGSV", 6) == 0 || strncmp(line, "$GNGSV", 6) == 0) {
        char copy[GPS_FEED_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        uint8_t sats_in_view = 0;
        if (nmea_parse_gsv(copy, &sats_in_view)) {
            f->sats_in_view = sats_in_view;
        }
    } else if (nmea_is_antenna_open(line)) {
        f->antenna_until_ms = now_ms + GPS_FEED_ANTENNA_TTL_MS;
    } else if (strncmp(line, "$PAIR021", 8) == 0 && f->chip_banner[0] == '\0') {
        strncpy(f->chip_banner, line, sizeof(f->chip_banner) - 1);
        f->chip_banner[sizeof(f->chip_banner) - 1] = '\0';
    }

    if (parsed && f->acc.valid) {
        f->pos.latitude_e7 = f->acc.latitude_e7;
        f->pos.longitude_e7 = f->acc.longitude_e7;
        f->pos.altitude_m = f->acc.altitude_m;
        f->pos.accuracy_m = f->acc.accuracy_m;
        f->pos.speed_kmh = f->acc.speed_kmh;
        f->pos.heading_deg2 = f->acc.heading_deg2;
        f->pos.timestamp = (uint32_t)(now_ms / 1000);
        f->pos.valid = true;
        f->has_fix = true;

        if (f->acc.utc_valid) {
            f->utc_hour = f->acc.utc_hour;
            f->utc_min = f->acc.utc_min;
            f->utc_valid = true;
        }

        if (f->cb) {
            f->cb(&f->pos, f->cb_ctx);
        }
    }
}

void gps_feed_bytes(gps_feed_t* f, const uint8_t* data, size_t len, uint64_t now_ms) {
    f->rx_bytes_total += (uint32_t)len;

    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '$') {
            f->line_pos = 0;
            f->line_buf[f->line_pos++] = c;
        } else if (c == '\n' || c == '\r') {
            if (f->line_pos > 0) {
                f->line_buf[f->line_pos] = '\0';
                f->rx_lines_total++;
                gps_feed_line(f, f->line_buf, now_ms);
                f->line_pos = 0;
            }
        } else if (f->line_pos < GPS_FEED_MAX_LINE - 1) {
            f->line_buf[f->line_pos++] = c;
        }
    }
}

bool gps_feed_has_fix(const gps_feed_t* f) { return f->has_fix; }

bool gps_feed_get_position(const gps_feed_t* f, bramble_position_t* out) {
    if (!f->has_fix)
        return false;
    *out = f->pos;
    return true;
}

bool gps_feed_get_utc_hm(const gps_feed_t* f, uint8_t* hour, uint8_t* min) {
    if (!f->utc_valid)
        return false;
    if (hour)
        *hour = f->utc_hour;
    if (min)
        *min = f->utc_min;
    return true;
}

void gps_feed_get_stats(const gps_feed_t* f, uint64_t now_ms, gps_stats_t* out) {
    out->sats_used = f->sats_used;
    out->sats_in_view = f->sats_in_view;
    out->antenna_warning = (f->antenna_until_ms != 0) && (now_ms < f->antenna_until_ms);
}
