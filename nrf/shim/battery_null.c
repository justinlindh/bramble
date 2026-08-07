// Battery voltage stub for the WM1110 dev kit: it is USB-powered with no
// battery sense wired in P1, so report a steady healthy voltage;
// battery_pct.c (already linked) turns this into the pct beacons carry.
#include "battery.h"

void battery_init(void) {} /* no hardware to bring up */

uint32_t battery_read_mv(void) { return 4000; }

uint8_t battery_read_pct(void) { return battery_mv_to_pct(battery_read_mv()); }
