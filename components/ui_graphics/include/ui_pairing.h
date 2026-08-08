#ifndef BRAMBLE_UI_PAIRING_H
#define BRAMBLE_UI_PAIRING_H

#include <stdint.h>
#include <stdbool.h>

/*
 * BLE pairing code modal. ui_pairing_passkey_cb matches
 * ble_passkey_display_cb_t and is registered with the BLE server in main.c;
 * it fires on the NimBLE host task, so it only records the request and an
 * LVGL timer owned by this module applies it on the LVGL task. show=true
 * displays the 6-digit code full screen; show=false dismisses it.
 */
void ui_pairing_init(void); /* call from ui_graphics_init (LVGL task context) */
void ui_pairing_passkey_cb(uint32_t passkey, bool show); /* safe from any task */

#endif
