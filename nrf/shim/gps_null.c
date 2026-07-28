// GPS null backend for the nRF52840 target: the dev kit routes GNSS through
// the LR1110 (P3 work), so until then the mesh sees a GPS-less node, exactly
// like a Heltec V3. Satisfies components/gps/include/gps.h.
#include "gps.h"

#include <string.h>

int gps_init(gps_fix_cb_t cb, void* ctx) {
    (void)cb;
    (void)ctx;
    return 0;
}

bool gps_has_fix(void) { return false; }

bool gps_get_position(bramble_position_t* out) {
    (void)out;
    return false;
}

bool gps_get_utc_hm(uint8_t* hour, uint8_t* min) {
    (void)hour;
    (void)min;
    return false;
}

void gps_get_stats(gps_stats_t* out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

int gps_set_enabled(bool enabled) {
    (void)enabled;
    return 0;
}

void gps_deinit(void) {}
