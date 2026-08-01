/* nrf/src/gps_events.h */
#pragma once
#include "gps.h"

/* Fix callback for gps_init: emits bramble.onGpsEvent fix_acquired,
 * throttled to one per 5s (parity with main.c's on_gps_fix). */
void nrf_on_gps_fix(const bramble_position_t* pos, void* ctx);

/* Emit fix_lost; the driver calls this when power-down clears a fix. */
void nrf_gps_emit_fix_lost(void);
