#ifndef UI_SHARED_STATE_H
#define UI_SHARED_STATE_H

#include "routing.h"
#include "airtime_budget.h"
#include "location.h"
#include "gnss_status.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    neighbor_table_t neighbors;
    uint32_t beacon_tx_count;
    uint32_t beacon_rx_count;
    uint32_t packets_tx;
    uint32_t packets_rx;
    bool radio_ok;
    int16_t last_rx_rssi;
    int8_t last_rx_snr;
    airtime_budget_t airtime;
} ui_mesh_state_t;

const ui_mesh_state_t* ui_shared_mesh_state(void);
const location_manager_t* ui_shared_location_state(void);

/**
 * Fill a GNSS classifier input from the board capability, the GPS power
 * preference and one gps_get_stats() read. Shared by every screen that shows
 * GNSS state so the status bar, the map and the stats page cannot disagree.
 * gps_get_stats() takes the driver lock, so a caller takes one snapshot per
 * render rather than reading per field.
 * @param out: filled, must not be NULL
 */
void ui_shared_gnss_state(gnss_ui_input_t* out);

#endif
