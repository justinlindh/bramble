#ifndef BRAMBLE_MAP_ZOOM_H
#define BRAMBLE_MAP_ZOOM_H

#include <stdbool.h>
#include <stddef.h>

/* The Map screen's zoom ladder, kept free of LVGL so the arithmetic, the
 * on-screen readout and the scale bar are host-testable (scr_map.c itself is
 * not linkable off device; same split as chat_message_ui.c).
 *
 * A zoom level is the HORIZONTAL HALF-WIDTH of the visible window in km, so a
 * smaller number is zoomed further IN. MAP_ZOOM_AUTO is the fit-every-peer
 * mode the screen starts in; it is not an index into the ladder.
 *
 * A ladder index is transient: scr_map.c holds it in a static that resets on
 * reboot, and nothing writes it to NVS or ships it over RPC. Rungs may
 * therefore be added at either end without a stored index having to be
 * migrated. */

#define MAP_ZOOM_AUTO (-1)
#define MAP_ZOOM_LEVEL_COUNT 10

/* Half-width of the map canvas in pixels: one level's km spans exactly this
 * many pixels either side of centre. scr_map.c projects with it and the scale
 * bar sizes itself against it, so the two cannot drift apart. */
#define MAP_ZOOM_HALF_WIDTH_PX 140

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
 * "manual 50 m". The bare mode word alone names the feature without telling
 * anyone what it is set to. out_len should be at least 16. */
void map_zoom_format(char* out, size_t out_len, int idx, double km);

/* Scale bar for a zoom level: the round distance to draw and how many pixels
 * long it is, chosen so the bar stays in a legible band rather than spanning
 * the canvas or shrinking to a stub. Returns false when no distance fits, in
 * which case the caller draws no bar. Every ladder rung must yield one, which
 * test_map_zoom asserts, so extending the ladder means checking this too.
 * label_len should be at least 8. */
bool map_zoom_scale_bar(double zoom_km, int* out_px, char* out_label, size_t label_len);

#endif /* BRAMBLE_MAP_ZOOM_H */
