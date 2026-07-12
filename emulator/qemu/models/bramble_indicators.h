/*
 * Bramble indicator bridge + LEDC buzzer model (Phase 2 emulator).
 * See hw/xtensa/bramble/bramble_indicators.c.
 */

#ifndef HW_XTENSA_BRAMBLE_INDICATORS_H
#define HW_XTENSA_BRAMBLE_INDICATORS_H

#include "exec/memory.h"

/*
 * Attach the indicator bridge: overlay the LEDC window (so the buzzer tone is
 * observable) and register the GPIO OUT observer that forwards the pager's LED
 * (GPIO48) / vibra (GPIO16) / buzzer levels into an emu-link `ind` message,
 * reproducing indicator_virt.c for the QEMU node. Harmless with no emu-link
 * chardev (an `ind` send is then a no-op). Called from bramble_attach().
 *
 *   sys_mem  the system address space the LEDC overlay maps into.
 */
void bramble_indicators_attach(MemoryRegion *sys_mem);

#endif
