/*
 * Bramble GPIO observer/injector (P2.2).
 * See hw/xtensa/bramble_gpio.c.
 */

#ifndef HW_XTENSA_BRAMBLE_GPIO_H
#define HW_XTENSA_BRAMBLE_GPIO_H

#include "hw/qdev-core.h"
#include "exec/memory.h"

/*
 * Attach the Bramble GPIO observer/injector to the running esp32s3 machine.
 * Called from esp32s3_machine_init() where the SoC state pointer is in scope
 * (Esp32s3SocState is private to hw/xtensa/esp32s3.c, so we wire in at the
 * machine-init site rather than exporting SoC internals; see PHASE2.md P2.2).
 *
 *   sys_mem  the system address space the esp32s3 GPIO region lives in.
 *   intc     the interrupt matrix device, so a synthetic button press can
 *            raise ETS_GPIO_INTR_SOURCE like a real edge would.
 */
void bramble_gpio_attach(MemoryRegion *sys_mem, DeviceState *intc);

#endif
