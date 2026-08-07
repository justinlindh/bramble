// Battery voltage stub for the WM1110 dev kit: it is USB-powered with no
// battery sense wired in P1, so report a steady healthy voltage;
// battery_pct.c (already linked) turns this into the pct beacons carry.
#include "battery.h"
#include "battery_nrf.h"

#include "bramble_board.h"

// Selection guard: the battery shim is chosen BY FILENAME in
// nrf/CMakeLists.txt, so nothing at link time notices a board whose divider
// needs the sensor-rail gate being built against a backend that never
// drives it; the result would compile clean and read a dead divider on
// hardware. Fail the build instead.
#ifdef BOARD_BATTERY_NEEDS_RAIL_GATE
#error                                                                                             \
    "this board's battery divider is rail-gated; battery_null.c cannot read it (fix the battery source selection in nrf/CMakeLists.txt)"
#endif

void battery_init(void) {} /* no hardware to bring up */

void battery_runtime_arm(void) {} /* nothing gated, nothing to arm */

uint8_t battery_probe_state(void) { return 0; } /* no rail, no probe */

uint32_t battery_read_mv(void) { return 4000; }

uint8_t battery_read_pct(void) { return battery_mv_to_pct(battery_read_mv()); }
