/*
 * Bramble device-model fan-out (Phase 2 emulator).
 * See hw/xtensa/bramble/bramble_attach.c.
 */

#ifndef HW_XTENSA_BRAMBLE_ATTACH_H
#define HW_XTENSA_BRAMBLE_ATTACH_H

#include "hw/qdev-core.h"
#include "exec/memory.h"

/*
 * Single fan-out entry point for the Bramble Phase 2 emulator device models.
 * Called once from esp32s3_machine_init(), where the SoC state is in scope, so
 * every model is wired from one stable call site instead of a per-model patch
 * to the machine init (Esp32s3SocState is private to esp32s3.c, so the handles
 * the models need are threaded through these parameters rather than exported).
 * Attaches, in order: the injection scaffold, the GPIO observer/injector, the
 * SAR ADC oneshot stub, the GPSPI2 controller (with its SX1262 radio and SSD1680
 * display SSI slaves), the LEDC/indicator bridge, and the emu-link bridge.
 *
 *   sys_mem  the system address space the model overlays map into.
 *   gdma     the esp32s3 GDMA device (the GPSPI2 framebuffer DMA data path).
 *   intc     the interrupt matrix device (GPIO and GPSPI2 interrupt sources).
 */
void bramble_attach(MemoryRegion *sys_mem, DeviceState *gdma, DeviceState *intc);

#endif
