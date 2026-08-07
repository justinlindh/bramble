#ifndef BRAMBLE_GPS_H
#define BRAMBLE_GPS_H

#include <stdbool.h>
#include <stdint.h>
#include "location.h"

/**
 * GPS fix callback - called when a new valid position is available.
 * @param pos: pointer to position structure
 * @param ctx: user context pointer
 */
typedef void (*gps_fix_cb_t)(const bramble_position_t* pos, void* ctx);

/* Sentinel for gps_stats_t.nmea_age_s: no NMEA line has been received since
 * the feed was last reset (module silent, unpowered, or miswired). */
#define GPS_STATS_NMEA_NEVER UINT32_MAX

/**
 * Satellite counts, signal strength and antenna health, tracked independently
 * of position fix validity so a caller can show progress before the first fix
 * and tell a receiver hearing nothing from one hearing satellites but not yet
 * converging.
 */
typedef struct {
    uint8_t sats_used;    /* GGA field 7: satellites used in the fix computation, 0-99 */
    uint8_t sats_in_view; /* GSV field 3, summed across constellations, 0-99. One
                           * constellation's signal bands combine by maximum rather
                           * than by sum, since a satellite received on two bands is
                           * listed once per band and is still one satellite. */
    uint8_t sats_tracked; /* GSV entries reporting a nonzero C/N0, combined the same
                           * way, 0-99. Satellites the receiver is actually hearing,
                           * as opposed to ones the almanac predicts. */
    uint8_t snr_max_dbhz; /* Best C/N0 across all GSV entries in the last complete
                           * message cycle, dB-Hz, 0-99. 0 when nothing is tracked. */
    uint8_t fix_quality;  /* GGA field 6 as a digit: 0 invalid, 1 GPS, 2 DGPS,
                           * 4 RTK fixed, 5 RTK float, 6 dead reckoning. 0 when no
                           * GGA has been seen, and 0 for a digit outside the 0-8
                           * range NMEA 0183 defines. */
    uint32_t nmea_age_s;  /* Seconds since the first NMEA line after the last feed
                           * reset, or GPS_STATS_NMEA_NEVER when none has arrived. */
    bool antenna_warning; /* true if a $GPTXT ANTENNA OPEN message was seen recently */
} gps_stats_t;

/**
 * Initialize GPS UART and start background parsing task.
 * @param cb: callback function to notify on new fix (can be NULL)
 * @param ctx: user context pointer passed to callback
 * @return 0 on success, -1 on failure
 */
int gps_init(gps_fix_cb_t cb, void* ctx);

/**
 * Check if GPS currently has a valid fix.
 *
 * Answers "a fix is held" rather than "a fix was held at some point": it goes
 * false when the receiver reports its fix as invalid and when the receiver
 * stops sending NMEA altogether, which is what makes a node carried out of
 * coverage report losing the fix.
 *
 * @return true if valid fix available
 */
bool gps_has_fix(void);

/**
 * Get current GPS position.
 * @param out: pointer to position structure to fill
 * @return true if valid position copied, false if no fix
 */
bool gps_get_position(bramble_position_t* out);

/**
 * Get the last known UTC wall-clock time-of-day from GPS (ground-truth UTC,
 * available the moment a node acquires a fix, no mesh timesync needed).
 * @param hour: filled with UTC hour 0-23 (may be NULL)
 * @param min: filled with UTC minute 0-59 (may be NULL)
 * @return true if a valid UTC time from a current fix is available
 */
bool gps_get_utc_hm(uint8_t* hour, uint8_t* min);

/**
 * Get the last known UTC calendar date from GPS. Reported separately from the
 * time of day because the two arrive in different NMEA sentences: only RMC
 * carries a date, so a receiver can have a valid time of day before it has a
 * date. Rendering local time needs the date, since a daylight-saving rule
 * cannot be evaluated without knowing the day of the year.
 * @param year: filled with the full year, e.g. 2026 (may be NULL)
 * @param month: filled with the month 1-12 (may be NULL)
 * @param day: filled with the day of month 1-31 (may be NULL)
 * @return true if a valid UTC date from a current fix is available
 */
bool gps_get_utc_date(uint16_t* year, uint8_t* month, uint8_t* day);

/**
 * Get current satellite counts and antenna health.
 * Always fills out, even when no position fix is available.
 * @param out: pointer to stats structure to fill
 */
void gps_get_stats(gps_stats_t* out);

/**
 * Raw-feed diagnostics: byte/line counters and the chip identification
 * banner, for telling "UART dead" from "flowing but unparseable" on a
 * console-less board.
 */
typedef struct {
    uint32_t rx_bytes_total;
    uint32_t rx_lines_total;
    char chip[64];          /* first $PAIR021* banner line (truncated), "" if none */
    uint32_t rx_overruns;   /* bytes dropped because an internal buffer was full; 0 on
                             * backends without one (currently nRF only) */
    uint32_t rx_errors;     /* UART/driver error events observed; 0 on backends without
                             * one (currently nRF only) */
    uint32_t rx_disabled;   /* times the driver silently disabled the receiver and had
                             * to be restarted; 0 on backends without one (nRF only) */
    uint32_t rx_rearm_fail; /* failed attempts to hand the driver a receive buffer; 0 on
                             * backends without one (nRF only) */
} gps_debug_t;

/**
 * Get raw-feed diagnostics (rx byte/line counters, chip banner).
 * Zeroed when the board has no GPS or the driver never started.
 * @param out: pointer to debug structure to fill
 */
void gps_get_debug(gps_debug_t* out);

/**
 * Enable or disable GPS at runtime.
 *
 * Enabling powers the GNSS on and (re)starts parsing, reusing the fix callback
 * registered by a prior gps_init(); disabling cuts GNSS power. This is the same
 * physical seam as gps_init()/gps_deinit() (the pager's GPIO38 P-FET gate, the
 * emulator's gpsgate message), exposed for the Settings toggle. A prior
 * gps_init() must have run so the callback is known.
 *
 * @param enabled true to power GNSS on, false to cut power
 * @return 0 on success, -1 if the board has no GPS
 */
int gps_set_enabled(bool enabled);

/**
 * Shutdown GPS and free resources.
 *
 * No other GPS API call may be in flight when this runs; callers must
 * quiesce their own users of the GPS API first. A query blocked inside the
 * driver during deinit is undefined: on the nRF driver, teardown deletes the
 * locks that a blocked query is waiting on.
 */
void gps_deinit(void);

#endif /* BRAMBLE_GPS_H */
