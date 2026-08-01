// Battery voltage stub for boards with no battery sense wired (the WM1110
// dev kit, BOARD_HAS_BATTERY 0): it is USB-powered. Wave 2 replaced the old
// fake "4000 mV, always healthy" reading with an honest "no battery
// hardware here" status (present=false, mv=0, pct=0, charging=UNKNOWN).
// getStatus/getBattery RPC consumers on these boards see 0 mV / not-present;
// the T1000-E gets the real reading instead, from shim/battery_saadc.c
// (selected in nrf/CMakeLists.txt by BRAMBLE_NRF_BOARD). A fabricated
// healthy number is worse than an honest zero (see honesty conventions in
// CLAUDE.md).
#include "battery.h"
#include <string.h>

/* Nothing to configure: no battery ADC or charge-detect pins on this board.
 * Exists so app_init.c can call battery_init() unconditionally, the same
 * way it does on every ESP board (main/main.c). */
void battery_init(void) {}

void battery_get_status(battery_status_t* out) { memset(out, 0, sizeof(*out)); }
