#include "map_zoom.h"
#include <stdio.h>

/* Roughly 1:2.5 between rungs: wide enough that a step is unmistakable on a
 * 302 px canvas, tight enough that walking the whole ladder is a few presses. */
static const double LEVELS_KM[MAP_ZOOM_LEVEL_COUNT] = {0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0};

static int clamp_index(int idx) {
    if (idx < 0)
        return 0;
    if (idx >= MAP_ZOOM_LEVEL_COUNT)
        return MAP_ZOOM_LEVEL_COUNT - 1;
    return idx;
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
    const char* mode = (idx == MAP_ZOOM_AUTO) ? "auto" : "manual";
    if (km >= 1.0)
        snprintf(out, out_len, "%s %g km", mode, km);
    else
        snprintf(out, out_len, "%s %d m", mode, (int)(km * 1000.0 + 0.5));
}
