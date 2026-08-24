/*
 * Bramble device-model fan-out (QEMU esp32s3).
 *
 * esp32s3_machine_init() makes ONE call, bramble_attach(), which wires in every
 * Bramble device model. That single call site is the point of this shim: adding
 * a model means dropping its source in this directory, listing it in
 * meson.build, and adding its attach call below, with no new patch against
 * stock QEMU. Giving each model its own patch to both hw/xtensa/meson.build and
 * the machine-init site makes every patch hand-placed to dodge its siblings'
 * git-apply context windows, which is O(N^2) fragile. The models stay
 * independent translation units, each with its own type_init; this file only
 * owns the attach ORDER and the handle plumbing.
 *
 * No vCPU runs during machine init, so the attach order below is not timing-
 * sensitive; it simply reads top-down in dependency order (shared GPIO overlay
 * first, then the peripherals that read/drive its pins, then the bridges that
 * forward their state to the ether).
 */

#include "qemu/osdep.h"
#include "hw/xtensa/bramble_attach.h"
#include "hw/xtensa/bramble_scaffold.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/xtensa/bramble_adc.h"
#include "hw/xtensa/bramble_gpspi2.h"
#include "hw/xtensa/bramble_indicators.h"
#include "hw/xtensa/bramble_emulink.h"

void bramble_attach(MemoryRegion *sys_mem, DeviceState *gdma, DeviceState *intc)
{
    /* Injection-scaffold proof: a log line at machine realize. */
    bramble_scaffold_init();

    /* GPIO observer/injector: the shared board pins (buttons, GNSS_EN, LED /
     * vibra outputs, the radio's soft CS, DIO1) that the peripherals below read
     * back and drive. Attached first so its accessors are live for them. */
    bramble_gpio_attach(sys_mem, intc);

    /* SAR ADC oneshot stub so battery_read_mv() completes. */
    bramble_adc_attach(sys_mem);

    /* GPSPI2 (SPI2_HOST) controller plus its two register-accurate SSI slaves,
     * the SX1262 radio and the SSD1680 e-paper. */
    bramble_gpspi2_attach(sys_mem, gdma, intc);

    /* LEDC buzzer overlay + the LED/vibra/buzzer -> emu-link `ind` bridge. */
    bramble_indicators_attach(sys_mem);

    /* Emu-link bridge to the gosim ether: connects the "emulink" chardev so the
     * SX1262 meshes with the linux pagers. A no-op on a standalone boot. */
    bramble_emulink_attach();
}
