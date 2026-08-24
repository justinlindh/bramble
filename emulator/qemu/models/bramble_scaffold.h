/*
 * Bramble device-model injection scaffold. See
 * hw/xtensa/bramble_scaffold.c.
 */

#ifndef HW_XTENSA_BRAMBLE_SCAFFOLD_H
#define HW_XTENSA_BRAMBLE_SCAFFOLD_H

#include "hw/qdev-core.h"
#include "exec/memory.h"

void bramble_scaffold_init(void);

/*
 * Attach a Bramble MemoryRegion overlay device to the running machine: give the
 * (already object_new'd) `obj` a canonical child path, realize it, and map its
 * `iomem` region over the machine's catch-all IO at `base` at higher priority
 * (subregion_overlap, priority 1), then log the one-time `banner` (the helper
 * appends " attached at 0x<base>"). Holds the boilerplate the GPIO / ADC / LEDC
 * / GPSPI2 overlays would otherwise each repeat.
 *
 * The caller keeps object_new (it needs the typed pointer for device-specific
 * wiring, and the helper needs that device's iomem to map). Any post-attach
 * wiring (interrupt lines, sibling-model hookups) happens in the caller after
 * this returns; the overlay is live but no vCPU runs during machine init, so
 * the ordering is immaterial.
 */
void bramble_overlay_attach(Object *obj, const char *child_name,
                            MemoryRegion *iomem, MemoryRegion *sys_mem,
                            hwaddr base, const char *banner);

#endif
