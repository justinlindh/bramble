/*
 * Bramble SSD1680 e-paper SSI slave (Phase 2 emulator).
 * See hw/xtensa/bramble/bramble_ssd1680.c.
 */

#ifndef HW_XTENSA_BRAMBLE_SSD1680_H
#define HW_XTENSA_BRAMBLE_SSD1680_H

/*
 * QOM type of the register-accurate SSD1680 display, instantiated on the GPSPI2
 * bus by bramble_gpspi2_attach. The type is registered by bramble_ssd1680.c's
 * own type_init, so the controller only needs this name to qdev_new it.
 */
#define TYPE_BRAMBLE_SSD1680 "bramble.ssd1680"

#endif
