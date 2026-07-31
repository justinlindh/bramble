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

/**
 * Satellite counts and antenna health, tracked independently of position
 * fix validity so a "searching" UI can show progress before the first fix.
 */
typedef struct {
    uint8_t sats_used;    /* GGA: satellites used in the most recent fix computation */
    uint8_t sats_in_view; /* GSV: total satellites currently in view */
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
    char chip[64];        /* first $PAIR021* banner line (truncated), "" if none */
    uint32_t rx_overruns; /* bytes dropped because an internal buffer was full; 0 on
                           * backends without one (currently nRF only) */
    uint32_t rx_errors;   /* UART/driver error events observed; 0 on backends without
                           * one (currently nRF only) */
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
 */
void gps_deinit(void);

#endif /* BRAMBLE_GPS_H */
