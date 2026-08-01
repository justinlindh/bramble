// Battery voltage stub for the WM1110 dev kit: it is USB-powered with no
// battery sense wired in P1. Wave 2 replaces the old fake "4000 mV, always
// healthy" reading with an honest "no battery hardware here" status
// (present=false, mv=0, pct=0, charging=UNKNOWN). getStatus/getBattery RPC
// consumers on the dev kit see 0 mV / not-present until the T1000-E's real
// SAADC + charge-detect backend lands (Task 4): a deliberate accuracy fix,
// not a regression. A fabricated healthy number is worse than an honest
// zero (see honesty conventions in CLAUDE.md).
#include "battery.h"
#include <string.h>

/* Nothing to configure: no battery ADC or charge-detect pins on this board.
 * Exists so app_init.c can call battery_init() unconditionally, the same
 * way it does on every ESP board (main/main.c). */
void battery_init(void) {}

void battery_get_status(battery_status_t* out) { memset(out, 0, sizeof(*out)); }
