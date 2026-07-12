/*
 * Bramble device-model injection scaffold (P2.2-infra).
 *
 * Placeholder for the P2.2-P2.5 Bramble device models (GPIO, GPSPI2,
 * SX1262, SSD1680; see emulator/PHASE2.md in the bramble repo). This file
 * exists only to prove the clone -> inject -> patch -> build -> observable
 * round trip those models will build on. It implements no device behavior.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "hw/xtensa/bramble_scaffold.h"

void bramble_scaffold_init(void)
{
    fprintf(stderr, "bramble: device scaffold active\n");
}

void bramble_overlay_attach(Object *obj, const char *child_name,
                            MemoryRegion *iomem, MemoryRegion *sys_mem,
                            hwaddr base, const char *banner)
{
    object_property_add_child(qdev_get_machine(), child_name, obj);
    qdev_realize(DEVICE(obj), NULL, &error_fatal);
    memory_region_add_subregion_overlap(sys_mem, base, iomem, 1);
    fprintf(stderr, "%s attached at 0x%x\n", banner, (unsigned)base);
}
