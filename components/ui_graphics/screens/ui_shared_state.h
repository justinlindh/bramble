#ifndef UI_SHARED_STATE_H
#define UI_SHARED_STATE_H

#include "routing.h"
#include "airtime_budget.h"
#include "location.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    neighbor_table_t neighbors;
    uint32_t         beacon_tx_count;
    uint32_t         beacon_rx_count;
    uint32_t         packets_tx;
    uint32_t         packets_rx;
    bool             radio_ok;
    int16_t          last_rx_rssi;
    int8_t           last_rx_snr;
    airtime_budget_t airtime;
} ui_mesh_state_t;

const ui_mesh_state_t *ui_shared_mesh_state(void);
const location_manager_t *ui_shared_location_state(void);

#endif
