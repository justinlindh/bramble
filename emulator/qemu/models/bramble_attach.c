/*
 * Bramble device-model fan-out (QEMU esp32s3, Phase 2 emulator).
 *
 * esp32s3_machine_init() makes ONE call, bramble_attach(), which wires in every
 * Bramble device model. Previously each model was injected by its own patch that
 * edited both hw/xtensa/meson.build and this machine-init site, hand-placed to
 * dodge its siblings' git-apply context windows; that O(N^2) fragility is why
 * this shim exists. Adding a model is now: drop its source in this directory,
 * list it in meson.build, and add its attach call below - no new patch to stock
 * QEMU. The models stay independent translation units, each with its own
 * type_init; this file only owns the attach ORDER and the handle plumbing.
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
    /* Injection-scaffold proof: a log line at machine realize (P2.2-infra). */
    bramble_scaffold_init();

    /* GPIO observer/injector: the shared board pins (buttons, GNSS_EN, LED /
     * vibra outputs, the radio's soft CS, DIO1) that the peripherals below read
     * back and drive. Attached first so its accessors are live for them. */
    bramble_gpio_attach(sys_mem, intc);

    /* SAR ADC oneshot stub so battery_read_mv() completes (P2.3b). */
    bramble_adc_attach(sys_mem);

    /* GPSPI2 (SPI2_HOST) controller plus its two register-accurate SSI slaves,
     * the SX1262 radio (P2.4) and the SSD1680 e-paper (P2.5). */
    bramble_gpspi2_attach(sys_mem, gdma, intc);

    /* LEDC buzzer overlay + the LED/vibra/buzzer -> emu-link `ind` bridge. */
    bramble_indicators_attach(sys_mem);

    /* Emu-link bridge to the gosim ether: connects the "emulink" chardev so the
     * SX1262 meshes with the linux pagers. A no-op on a standalone boot. */
    bramble_emulink_attach();
}
