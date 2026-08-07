// Battery voltage stub for boards with no battery sense wired (the WM1110
// dev kit, BOARD_HAS_BATTERY 0): it is USB-powered. Reports an honest "no
// battery hardware here" status (present=false, mv=0, pct=0,
// charging=UNKNOWN) rather than a fabricated healthy reading.
// getStatus/getBattery RPC consumers on these boards see 0 mV / not-present.
// The T1000-E reports the same absent voltage from shim/battery_t1000e.c
// (selected in nrf/CMakeLists.txt by BRAMBLE_NRF_BOARD) for a different
// reason, and adds charge detection. A fabricated healthy number is worse
// than an honest zero (see honesty conventions in CLAUDE.md).
#include "battery.h"
#include <string.h>

/* Nothing to configure: no battery ADC or charge-detect pins on this board.
 * Exists so app_init.c can call battery_init() unconditionally, the same
 * way it does on every ESP board (main/main.c). */
void battery_init(void) {}

void battery_get_status(battery_status_t* out) { memset(out, 0, sizeof(*out)); }
