/*
 * Bramble device-model injection scaffold.
 *
 * Shared plumbing for the Bramble device models (GPIO, GPSPI2, SX1262,
 * SSD1680; see docs/archive/plans/emulator-phase2-qemu-spec.md in the bramble
 * repo): a machine-init banner that makes the injected models observable, and
 * bramble_overlay_attach, the helper each overlay model uses to map its
 * register window over the machine's catch-all IO region. It models no device
 * of its own.
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
