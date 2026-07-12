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

/*
 * Current driven level (0/1) of GPIO output pin `pin` (0..48), read from the
 * overlay's OUT registers. Lets bramble_gpspi2 route SPI transfers by the
 * radio's manual chip-select on GPIO8 without duplicating GPIO state. Returns
 * false if the overlay is not attached or the pin is out of range.
 */
bool bramble_gpio_out_level(int pin);

/*
 * Drive INPUT pin `pin` (0..48) to `level` from a sibling device model,
 * latching GPIO_STATUS and raising ETS_GPIO_INTR_SOURCE on a rising (posedge)
 * edge. The SX1262 model (bramble_gpspi2.c) uses this to assert DIO1 (GPIO14)
 * for TxDone/RxDone; drive it back to 0 after the driver clears the IRQ to
 * re-arm the next edge. No-op if the overlay is not attached.
 */
void bramble_gpio_set_input(int pin, bool level);

#endif
