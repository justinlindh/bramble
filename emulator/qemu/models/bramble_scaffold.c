/*
 * Bramble device-model injection scaffold (P2.2-infra).
 *
 * Placeholder for the P2.2-P2.5 Bramble device models (GPIO, GPSPI2,
 * SX1262, SSD1680; see emulator/PHASE2.md in the bramble repo). This file
 * exists only to prove the clone -> inject -> patch -> build -> observable
 * round trip those models will build on. It implements no device behavior.
 */

#include "qemu/osdep.h"
#include "hw/xtensa/bramble_scaffold.h"

void bramble_scaffold_init(void)
{
    fprintf(stderr, "bramble: device scaffold active\n");
}
