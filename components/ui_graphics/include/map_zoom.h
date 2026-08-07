#ifndef BRAMBLE_MAP_ZOOM_H
#define BRAMBLE_MAP_ZOOM_H

#include <stdbool.h>
#include <stddef.h>

/* The Map screen's zoom ladder, kept free of LVGL so the arithmetic and the
 * on-screen readout are host-testable (scr_map.c itself is not linkable off
 * device; same split as chat_message_ui.c).
 *
 * A zoom level is the HORIZONTAL HALF-WIDTH of the visible window in km, so a
 * smaller number is zoomed further IN. MAP_ZOOM_AUTO is the fit-every-peer
 * mode the screen starts in; it is not an index into the ladder. */

#define MAP_ZOOM_AUTO (-1)
#define MAP_ZOOM_LEVEL_COUNT 7

/* Half-width in km for a ladder index, clamped, so an out-of-range index
 * yields the nearest real level rather than reading off the end. */
double map_zoom_level_km(int idx);

/* Lowest ladder index whose window is at least km wide (the widest level when
 * nothing on the ladder reaches it). */
int map_zoom_index_for_km(double km);

/* One notch: step < 0 zooms in, step > 0 zooms out. Leaving MAP_ZOOM_AUTO
 * starts from the level auto currently resolves to (auto_km), so the first
 * press is a single visible step rather than a jump to the end of the ladder.
 * Clamped at both ends; returns the resulting index. */
int map_zoom_step(int cur, int step, double auto_km);

/* Would that notch change the zoom state? Drives the dimmed look of the
 * on-screen +/- controls and lets a handler skip a no-op rebuild. Leaving
 * auto always counts: it pins the window even when the level is unchanged. */
bool map_zoom_can_step(int cur, int step, double auto_km);

/* Zoom readout: the mode plus the level actually in force, e.g. "auto 5 km" or
 * "manual 500 m". The bare mode word alone names the feature without telling
 * anyone what it is set to. out_len should be at least 16. */
void map_zoom_format(char* out, size_t out_len, int idx, double km);

#endif /* BRAMBLE_MAP_ZOOM_H */
