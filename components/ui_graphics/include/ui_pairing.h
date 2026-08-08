#ifndef BRAMBLE_UI_PAIRING_H
#define BRAMBLE_UI_PAIRING_H

#include <stdint.h>
#include <stdbool.h>

/*
 * BLE pairing code modal. ui_pairing_passkey_cb matches
 * ble_passkey_display_cb_t and is registered with the BLE server in main.c;
 * it fires on the NimBLE host task, so it only records the request and
 * ui_pairing_poll(), driven by ui_graphics.c's existing 500ms timer, applies
 * it on the LVGL task. show=true displays the 6-digit code full screen;
 * show=false dismisses it.
 */
void ui_pairing_passkey_cb(uint32_t passkey, bool show); /* safe from any task */

/* Drains a pending show/hide request. LVGL task context only; called from
 * ui_graphics.c's 500ms timer callback. 500ms worst-case show/clear latency
 * is negligible against NimBLE's ~30s SM pairing timeout. */
void ui_pairing_poll(void);

#endif
