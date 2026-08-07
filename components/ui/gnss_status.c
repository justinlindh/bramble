#include "gnss_status.h"
#include <stdarg.h>
#include <stdio.h>

/* snprintf reports the length it WOULD have written; callers of this file want
 * the length actually in the buffer, so truncation is folded back here. */
__attribute__((format(printf, 3, 4))) static int emit(char* out, size_t out_len, const char* fmt,
                                                      ...) {
    if (!out || out_len == 0)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, out_len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n >= out_len ? (int)(out_len - 1) : n;
}

/* Clamp to two digits so a fixed-width badge stays two characters wide. */
static unsigned clamp99(unsigned v) { return v > 99 ? 99 : v; }

gnss_ui_state_t gnss_ui_classify(const gnss_ui_input_t* in) {
    if (!in || !in->board_has_gnss || !in->powered)
        return GNSS_UI_ABSENT;
    /* has_fix is the driver's live answer, not a latch: gps_feed drops it when
     * the receiver reports no fix and when the receiver stops talking, so a
     * node carried from a place it fixed to a place it hears nothing falls
     * through to the classes below. */
    if (in->has_fix)
        return GNSS_UI_FIX;
    /* sats_used covers a receiver with GSV disabled, where a nonzero GGA
     * satellite count is the only evidence that signal is present. */
    if (in->sats_tracked > 0 || in->sats_used > 0)
        return GNSS_UI_ACQUIRING;
    /* A module that has never said a word is the most severe case and must
     * not be hidden by the warmup grace below. */
    if (in->nmea_age_s == GNSS_UI_NMEA_NEVER)
        return GNSS_UI_NO_SIGNAL;
    if (in->nmea_age_s < GNSS_UI_WARMUP_S)
        return GNSS_UI_ACQUIRING;
    return GNSS_UI_NO_SIGNAL;
}

const char* gnss_ui_state_label(gnss_ui_state_t state) {
    switch (state) {
    case GNSS_UI_FIX:
        return "fix";
    case GNSS_UI_ACQUIRING:
        return "acquiring";
    case GNSS_UI_NO_SIGNAL:
        return "no signal";
    case GNSS_UI_ABSENT:
    default:
        return "off";
    }
}

const char* gnss_ui_state_wire(gnss_ui_state_t state) {
    switch (state) {
    case GNSS_UI_FIX:
        return "fix";
    case GNSS_UI_ACQUIRING:
        return "acquiring";
    case GNSS_UI_NO_SIGNAL:
        return "no_signal";
    case GNSS_UI_ABSENT:
    default:
        return "absent";
    }
}

int gnss_ui_badge_count(const gnss_ui_input_t* in, char* out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    out[0] = '\0';
    if (!in)
        return 0;

    switch (gnss_ui_classify(in)) {
    case GNSS_UI_ABSENT:
        return 0;
    case GNSS_UI_NO_SIGNAL:
        return emit(out, out_len, "--");
    case GNSS_UI_ACQUIRING:
        return emit(out, out_len, "%2u",
                    clamp99(in->sats_tracked ? in->sats_tracked : in->sats_used));
    case GNSS_UI_FIX:
    default:
        return emit(out, out_len, "%2u", clamp99(in->sats_used));
    }
}

int gnss_ui_detail_line(const gnss_ui_input_t* in, char* out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    out[0] = '\0';
    if (!in)
        return 0;

    switch (gnss_ui_classify(in)) {
    case GNSS_UI_ABSENT:
        return emit(out, out_len, "GNSS off");
    case GNSS_UI_NO_SIGNAL:
        if (in->sats_in_view == 0)
            return emit(out, out_len, "no satellites detected");
        return emit(out, out_len, "0 tracked of %u in view", clamp99(in->sats_in_view));
    case GNSS_UI_ACQUIRING:
        if (in->snr_max_dbhz > 0) {
            return emit(out, out_len, "%u tracked of %u in view, best %u dBHz",
                        clamp99(in->sats_tracked), clamp99(in->sats_in_view),
                        clamp99(in->snr_max_dbhz));
        }
        return emit(out, out_len, "%u used, %u in view", clamp99(in->sats_used),
                    clamp99(in->sats_in_view));
    case GNSS_UI_FIX:
    default:
        return emit(out, out_len, "%u used of %u in view, best %u dBHz", clamp99(in->sats_used),
                    clamp99(in->sats_in_view), clamp99(in->snr_max_dbhz));
    }
}
