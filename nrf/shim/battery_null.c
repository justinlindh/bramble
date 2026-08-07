// Battery voltage stub for boards with no battery sense wired (the WM1110
// dev kit, BOARD_HAS_BATTERY 0): it is USB-powered. Reports an honest "no
// battery hardware here" status (present=false, mv=0, pct=0,
// charging=UNKNOWN) rather than a fabricated healthy reading.
// getStatus/getBattery RPC consumers on these boards see 0 mV / not-present;
// the T1000-E gets the real reading instead, from shim/battery_t1000e.c
// (selected in nrf/CMakeLists.txt by BRAMBLE_NRF_BOARD). A fabricated
// healthy number is worse than an honest zero (see honesty conventions in
// CLAUDE.md).
#include "battery.h"
#include "battery_nrf.h"

#include <string.h>

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

/* Nothing to configure: no battery ADC or charge-detect pins on this board.
 * Exists so app_init.c can call battery_init() unconditionally, the same
 * way it does on every ESP board (main/main.c). */
void battery_init(void) {}

void battery_runtime_arm(void) {} /* nothing gated, nothing to arm */

uint8_t battery_probe_state(void) { return 0; } /* no rail, no probe */

void battery_get_status(battery_status_t* out) { memset(out, 0, sizeof(*out)); }
