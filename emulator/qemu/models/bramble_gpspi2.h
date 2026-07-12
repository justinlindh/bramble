/*
 * Bramble GPSPI2 (SPI2_HOST) controller model (P2.3).
 * See hw/xtensa/bramble_gpspi2.c.
 */

#ifndef HW_XTENSA_BRAMBLE_GPSPI2_H
#define HW_XTENSA_BRAMBLE_GPSPI2_H

#include "hw/qdev-core.h"
#include "exec/memory.h"

/*
 * Attach the Bramble GPSPI2 controller (plus a stub SSI slave) to the running
 * esp32s3 machine. Called from esp32s3_machine_init() where the SoC state
 * pointer is in scope (Esp32s3SocState is private to hw/xtensa/esp32s3.c, so we
 * wire in at the machine-init site rather than exporting SoC internals, exactly
 * like bramble_gpio_attach; see PHASE2.md P2.3).
 *
 *   sys_mem  the system address space the GPSPI2 register window lives in.
 *   gdma     the esp32s3 GDMA device, so the peripheral's DMA data path can
 *            move framebuffer bytes to/from guest RAM through the SPI2 channel.
 *   intc     the interrupt matrix device (ETS_SPI2_INTR_SOURCE); wired for
 *            completeness even though the pager drivers poll SPI_USR rather
 *            than take the transfer-done interrupt.
 */
void bramble_gpspi2_attach(MemoryRegion *sys_mem, DeviceState *gdma,
                           DeviceState *intc);

#endif
