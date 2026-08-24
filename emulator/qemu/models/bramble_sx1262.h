/*
 * Bramble SX1262 LoRa radio SSI slave. See
 * hw/xtensa/bramble/bramble_sx1262.c.
 */

#ifndef HW_XTENSA_BRAMBLE_SX1262_H
#define HW_XTENSA_BRAMBLE_SX1262_H

/*
 * QOM type of the register-accurate SX1262 radio, instantiated on the GPSPI2 bus
 * by bramble_gpspi2_attach. The type is registered by bramble_sx1262.c's own
 * type_init, so the controller only needs this name to qdev_new it.
 */
#define TYPE_BRAMBLE_SX1262 "bramble.sx1262"

/*
 * Radio manual software chip-select = GPIO8 (sx1262.c sets spics_io_num = -1 and
 * toggles gpio_set_level(8) by hand); GPIO8 low selects the SX1262 in the
 * controller's CS routing (bramble_gpspi2_route reads it back via the GPIO
 * overlay). Shared with the controller, which routes transfers by this pin.
 */
#define SX1262_CS_GPIO 8

#endif
