#include "map_zoom.h"
#include <stdio.h>

/* Roughly 1:2 to 1:2.5 between rungs: wide enough that a step is unmistakable
 * on a 302 px canvas, tight enough that no rung is a wasted press.
 *
 * The tight end stops at a 50 m half-width. Reported GPS accuracy on these
 * nodes runs about 4 to 6 m, which at that level is some 11 to 17 px of
 * scatter on a 280 px span: still small against real separation. A tighter
 * rung would mostly magnify position noise. */
static const double LEVELS_KM[MAP_ZOOM_LEVEL_COUNT] = {0.05, 0.1, 0.25, 0.5,  1.0,
                                                       2.0,  5.0, 10.0, 25.0, 50.0};

/* Round distances the scale bar is willing to draw, widest first, reaching far
 * enough down that the tightest ladder rung still finds one. */
static const double NICE_KM[] = {50.0, 20.0, 10.0, 5.0, 2.0, 1.0, 0.5, 0.2, 0.1, 0.05, 0.02};

/* A bar shorter than this reads as a tick, longer and it crowds the canvas. */
#define SCALE_BAR_MIN_PX 20
#define SCALE_BAR_MAX_PX 90

static int clamp_index(int idx) {
    if (idx < 0)
        return 0;
    if (idx >= MAP_ZOOM_LEVEL_COUNT)
        return MAP_ZOOM_LEVEL_COUNT - 1;
    return idx;
}

/* A distance the way this screen says it: kilometres above 1 km, whole metres
 * below. Rounded, not truncated, because a rung like 0.02 km is not exactly
 * representable and truncation would print it as 19 m. */
static void format_km(char* out, size_t out_len, double km) {
    if (km >= 1.0)
        snprintf(out, out_len, "%g km", km);
    else
        snprintf(out, out_len, "%d m", (int)(km * 1000.0 + 0.5));
}

double map_zoom_level_km(int idx) { return LEVELS_KM[clamp_index(idx)]; }

int map_zoom_index_for_km(double km) {
    for (int i = 0; i < MAP_ZOOM_LEVEL_COUNT; i++) {
        if (LEVELS_KM[i] >= km)
            return i;
    }
    return MAP_ZOOM_LEVEL_COUNT - 1;
}

int map_zoom_step(int cur, int step, double auto_km) {
    int idx = (cur == MAP_ZOOM_AUTO) ? map_zoom_index_for_km(auto_km) : clamp_index(cur);
    return clamp_index(idx + step);
}

bool map_zoom_can_step(int cur, int step, double auto_km) {
    return map_zoom_step(cur, step, auto_km) != cur;
}

void map_zoom_format(char* out, size_t out_len, int idx, double km) {
    if (!out || out_len == 0)
        return;
    char dist[12];
    format_km(dist, sizeof(dist), km);
    snprintf(out, out_len, "%s %s", (idx == MAP_ZOOM_AUTO) ? "auto" : "manual", dist);
}

bool map_zoom_scale_bar(double zoom_km, int* out_px, char* out_label, size_t label_len) {
    if (zoom_km <= 0.0)
        return false;
    double px_per_km = (double)MAP_ZOOM_HALF_WIDTH_PX / zoom_km;
    for (size_t i = 0; i < sizeof(NICE_KM) / sizeof(NICE_KM[0]); i++) {
        int px = (int)(NICE_KM[i] * px_per_km);
        if (px > SCALE_BAR_MAX_PX || px < SCALE_BAR_MIN_PX)
            continue;
        if (out_px)
            *out_px = px;
        if (out_label && label_len)
            format_km(out_label, label_len, NICE_KM[i]);
        return true;
    }
    return false;
}
