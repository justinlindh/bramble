#include "gps_feed.h"
#include "nmea_parser.h"
#include <ctype.h>
#include <string.h>

void gps_feed_init(gps_feed_t* f, gps_fix_cb_t cb, void* ctx) {
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->cb_ctx = ctx;
}

void gps_feed_clear_fix(gps_feed_t* f) {
    f->has_fix = false;
    f->utc_valid = false;
    f->utc_date_valid = false;
}

void gps_feed_reset(gps_feed_t* f) {
    gps_fix_cb_t cb = f->cb;
    void* cb_ctx = f->cb_ctx;
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->cb_ctx = cb_ctx;
}

/* Find the slot for a talker and signal id, else claim a free one, else
 * reclaim the one whose committed values expire soonest. NULL only when
 * talker is malformed. */
static gps_gsv_slot_t* gsv_slot_for(gps_feed_t* f, const char* talker, uint8_t signal_id) {
    if (!talker || talker[0] == '\0')
        return NULL;

    gps_gsv_slot_t* oldest = NULL;
    for (int i = 0; i < GPS_FEED_MAX_GNSS; i++) {
        gps_gsv_slot_t* s = &f->gsv[i];
        if (s->talker[0] != '\0' && s->signal_id == signal_id && strcmp(s->talker, talker) == 0)
            return s;
        if (!oldest || s->until_ms < oldest->until_ms)
            oldest = s;
    }
    for (int i = 0; i < GPS_FEED_MAX_GNSS; i++) {
        if (f->gsv[i].talker[0] == '\0') {
            memset(&f->gsv[i], 0, sizeof(f->gsv[i]));
            strncpy(f->gsv[i].talker, talker, sizeof(f->gsv[i].talker) - 1);
            f->gsv[i].signal_id = signal_id;
            return &f->gsv[i];
        }
    }
    if (oldest) {
        memset(oldest, 0, sizeof(*oldest));
        strncpy(oldest->talker, talker, sizeof(oldest->talker) - 1);
        oldest->signal_id = signal_id;
    }
    return oldest;
}

/* True while the receiver is still emitting NMEA. Everything the receiver
 * asserted stops being evidence once it stops talking. */
static bool feed_live(const gps_feed_t* f, uint64_t now_ms) {
    return f->any_line && now_ms < f->last_line_ms + GPS_FEED_SILENCE_TTL_MS;
}

void gps_feed_line(gps_feed_t* f, const char* line, uint64_t now_ms) {
    bool parsed = false;

    if (!f->any_line) {
        f->any_line = true;
        f->first_line_ms = now_ms;
    }
    f->last_line_ms = now_ms;

    if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        char copy[GPS_FEED_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        parsed = nmea_parse_rmc(copy, &f->acc);
        /* A receiver reporting status 'V' is telling the caller it holds no
         * fix, which is distinct from a line that failed to parse. Believing
         * it is what lets a node that acquired a fix somewhere else report
         * losing it. */
        if (!parsed && nmea_reports_no_fix(line))
            gps_feed_clear_fix(f);
    } else if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
        char copy[GPS_FEED_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        parsed = nmea_parse_gga(copy, &f->acc);
        /* Satellites-used and the receiver's own fix-quality verdict are both
         * reported with or without a fix. */
        f->sats_used = f->acc.sats_used;
        f->fix_quality = f->acc.fix_quality;
        if (!parsed && nmea_reports_no_fix(line))
            gps_feed_clear_fix(f);
    } else if (line[0] == '$' && isalpha((unsigned char)line[1]) &&
               isalpha((unsigned char)line[2]) && strncmp(line + 3, "GSV", 3) == 0) {
        char copy[GPS_FEED_MAX_LINE];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        nmea_gsv_t g;
        if (nmea_parse_gsv(copy, &g)) {
            gps_gsv_slot_t* s = gsv_slot_for(f, g.talker, g.signal_id);
            if (s) {
                if (g.msg_num <= 1) {
                    /* Start of this cycle: drop what the previous one through
                     * the same slot was accumulating. Numbering restarts per
                     * signal band, which is why the slot key includes the
                     * signal id: one band's cycle must not reset another's. */
                    s->acc_tracked = 0;
                    s->acc_snr_max = 0;
                }
                s->acc_tracked =
                    (uint8_t)((s->acc_tracked + g.tracked > 99) ? 99 : s->acc_tracked + g.tracked);
                if (g.snr_max > s->acc_snr_max)
                    s->acc_snr_max = g.snr_max;
                s->in_view = g.sats_in_view;
                s->until_ms = now_ms + GPS_FEED_GSV_TTL_MS;
                if (g.total_msgs == 0 || g.msg_num >= g.total_msgs) {
                    s->tracked = s->acc_tracked;
                    s->snr_max = s->acc_snr_max;
                }
            }
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

        if (f->acc.utc_date_valid) {
            f->utc_year = f->acc.utc_year;
            f->utc_month = f->acc.utc_month;
            f->utc_day = f->acc.utc_day;
            f->utc_date_valid = true;
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

bool gps_feed_has_fix(const gps_feed_t* f, uint64_t now_ms) {
    return f->has_fix && feed_live(f, now_ms);
}

bool gps_feed_get_position(const gps_feed_t* f, uint64_t now_ms, bramble_position_t* out) {
    if (!gps_feed_has_fix(f, now_ms))
        return false;
    *out = f->pos;
    return true;
}

bool gps_feed_get_utc_hm(const gps_feed_t* f, uint64_t now_ms, uint8_t* hour, uint8_t* min) {
    if (!f->utc_valid || !feed_live(f, now_ms))
        return false;
    if (hour)
        *hour = f->utc_hour;
    if (min)
        *min = f->utc_min;
    return true;
}

bool gps_feed_get_utc_date(const gps_feed_t* f, uint64_t now_ms, uint16_t* year, uint8_t* month,
                           uint8_t* day) {
    if (!f->utc_date_valid || !feed_live(f, now_ms))
        return false;
    if (year)
        *year = f->utc_year;
    if (month)
        *month = f->utc_month;
    if (day)
        *day = f->utc_day;
    return true;
}

void gps_feed_get_stats(const gps_feed_t* f, uint64_t now_ms, gps_stats_t* out) {
    /* memset rather than field-by-field so a member added later defaults to
     * zero instead of leaking whatever the caller's stack held. */
    memset(out, 0, sizeof(*out));
    /* The GGA-derived counts are only evidence while the receiver is talking:
     * a module that went silent keeps neither its satellites-used count nor
     * its fix-quality verdict. */
    if (feed_live(f, now_ms)) {
        out->sats_used = f->sats_used;
        out->fix_quality = f->fix_quality;
    }
    out->antenna_warning = (f->antenna_until_ms != 0) && (now_ms < f->antenna_until_ms);
    out->nmea_age_s =
        f->any_line ? (uint32_t)((now_ms - f->first_line_ms) / 1000) : GPS_STATS_NMEA_NEVER;

    /* A receiver reports either one combined GN cycle or one cycle per
     * constellation. When a combined cycle is live it already covers every
     * constellation, so summing it with the per-constellation slots would
     * double count. */
    bool have_gn = false;
    unsigned gn_in_view = 0, gn_tracked = 0, gn_snr_max = 0;
    unsigned in_view = 0, tracked = 0, snr_max = 0;
    for (int i = 0; i < GPS_FEED_MAX_GNSS; i++) {
        const gps_gsv_slot_t* s = &f->gsv[i];
        if (s->talker[0] == '\0' || now_ms >= s->until_ms)
            continue;
        /* Fold each talker's signal bands together on the first live slot for
         * that talker, and skip the rest. One satellite is listed once per
         * band it is being received on, so the bands combine by maximum: a
         * sum would count a dual-band satellite twice, and taking only one
         * band would report the strong band's satellites as unheard whenever
         * a quiet band happened to be the one kept. */
        bool folded_already = false;
        for (int j = 0; j < i; j++) {
            const gps_gsv_slot_t* p = &f->gsv[j];
            if (p->talker[0] != '\0' && now_ms < p->until_ms && strcmp(p->talker, s->talker) == 0) {
                folded_already = true;
                break;
            }
        }
        if (folded_already)
            continue;

        unsigned t_in_view = 0, t_tracked = 0, t_snr_max = 0;
        for (int j = i; j < GPS_FEED_MAX_GNSS; j++) {
            const gps_gsv_slot_t* b = &f->gsv[j];
            if (b->talker[0] == '\0' || now_ms >= b->until_ms || strcmp(b->talker, s->talker) != 0)
                continue;
            if (b->in_view > t_in_view)
                t_in_view = b->in_view;
            if (b->tracked > t_tracked)
                t_tracked = b->tracked;
            if (b->snr_max > t_snr_max)
                t_snr_max = b->snr_max;
        }

        if (strcmp(s->talker, "GN") == 0) {
            have_gn = true;
            gn_in_view = t_in_view;
            gn_tracked = t_tracked;
            gn_snr_max = t_snr_max;
            continue;
        }
        in_view += t_in_view;
        tracked += t_tracked;
        if (t_snr_max > snr_max)
            snr_max = t_snr_max;
    }
    if (have_gn) {
        in_view = gn_in_view;
        tracked = gn_tracked;
        snr_max = gn_snr_max;
    }
    out->sats_in_view = (uint8_t)(in_view > 99 ? 99 : in_view);
    out->sats_tracked = (uint8_t)(tracked > 99 ? 99 : tracked);
    out->snr_max_dbhz = (uint8_t)(snr_max > 99 ? 99 : snr_max);
}
