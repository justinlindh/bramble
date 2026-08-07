#include "ui_shared_state.h"
#include "board_config.h"
#include "gps.h"
#include <string.h>

extern void mesh_get_state(ui_mesh_state_t* out);
extern void mesh_get_location_state(location_manager_t* out);
extern bool bramble_gps_enabled(void); /* persisted GPS power preference (main.c) */

const ui_mesh_state_t* ui_shared_mesh_state(void) {
    static ui_mesh_state_t s_state;
    mesh_get_state(&s_state);
    return &s_state;
}

const location_manager_t* ui_shared_location_state(void) {
    static location_manager_t s_state;
    mesh_get_location_state(&s_state);
    return &s_state;
}

void ui_shared_gnss_state(gnss_ui_input_t* out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->board_has_gnss = board_has_cap(BOARD_CAP_GPS);
    out->powered = bramble_gps_enabled();
    if (!out->board_has_gnss || !out->powered) {
        /* A receiver that is absent or unpowered has sent nothing, and the
         * zero default would otherwise read as a fresh feed inside the
         * classifier's warmup grace. */
        out->nmea_age_s = GNSS_UI_NMEA_NEVER;
        return;
    }
    gps_stats_t st;
    gps_get_stats(&st);
    out->has_fix = gps_has_fix();
    out->sats_in_view = st.sats_in_view;
    out->sats_tracked = st.sats_tracked;
    out->sats_used = st.sats_used;
    out->snr_max_dbhz = st.snr_max_dbhz;
    out->fix_quality = st.fix_quality;
    out->nmea_age_s = st.nmea_age_s;
}
