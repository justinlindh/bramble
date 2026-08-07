#ifndef BRAMBLE_GNSS_STATUS_H
#define BRAMBLE_GNSS_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Three-way GNSS state, computed once in firmware and consumed by every UI.
 *
 * A single "has fix" boolean cannot separate a receiver hearing nothing
 * (interference, dead antenna, unpowered module) from a receiver hearing
 * satellites and not yet converging, and those two demand opposite responses
 * from a field operator. This header deliberately depends on nothing but the
 * C standard library so the same classification links into the ESP, nRF and
 * host builds, and so the caller maps the returned enum onto its own color
 * tokens rather than this file knowing about any display stack.
 */

/* Warmup grace: a receiver that has been fed NMEA for less than this has not
 * had time to acquire anything, so reporting "no signal" would be noise. */
#define GNSS_UI_WARMUP_S 30

/* No NMEA line has been seen; mirrors GPS_STATS_NMEA_NEVER without pulling in gps.h. */
#define GNSS_UI_NMEA_NEVER UINT32_MAX

typedef enum {
    GNSS_UI_ABSENT = 0, /* no GNSS on this board, or powered off by preference */
    GNSS_UI_NO_SIGNAL,  /* powered, nothing being heard: failure class A */
    GNSS_UI_ACQUIRING,  /* powered, satellites heard, no fix: failure class B */
    GNSS_UI_FIX,        /* fix computed: class C */
} gnss_ui_state_t;

typedef struct {
    bool board_has_gnss;
    bool powered; /* the GPS power preference */
    bool has_fix;
    uint8_t sats_in_view;
    uint8_t sats_tracked;
    uint8_t sats_used;
    uint8_t snr_max_dbhz;
    uint8_t fix_quality;
    uint32_t nmea_age_s; /* GNSS_UI_NMEA_NEVER when the module has sent nothing */
} gnss_ui_input_t;

/**
 * Classify a GNSS snapshot into the three-way state (plus absent).
 * Satellites listed in view but reporting no carrier-to-noise ratio are
 * almanac predictions, not received signal, and classify as no-signal.
 * @param in: snapshot, must not be NULL
 * @return the state
 */
gnss_ui_state_t gnss_ui_classify(const gnss_ui_input_t* in);

/* Lowercase state word for a status line: "off", "no signal", "acquiring", "fix".
 * Never NULL. */
const char* gnss_ui_state_label(gnss_ui_state_t state);

/* Wire-protocol token for the RPC gps_state field: "absent", "no_signal",
 * "acquiring", "fix". Never NULL. Kept separate from the display label so a
 * copy edit to the UI cannot silently change the wire contract. */
const char* gnss_ui_state_wire(gnss_ui_state_t state);

/* Right-aligned two-character count for a fixed-width status-bar badge:
 * "--" when nothing is heard, "" when absent, otherwise the state's most
 * meaningful count. Fixed width so a SPACE_BETWEEN flex row does not reflow
 * every tick. Writes at most 3 bytes. Returns characters written. */
int gnss_ui_badge_count(const gnss_ui_input_t* in, char* out, size_t out_len);

/* One ASCII diagnostic line, at most 40 characters plus NUL. Returns
 * characters written. */
int gnss_ui_detail_line(const gnss_ui_input_t* in, char* out, size_t out_len);

#endif /* BRAMBLE_GNSS_STATUS_H */
