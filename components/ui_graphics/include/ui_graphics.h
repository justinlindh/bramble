#ifndef BRAMBLE_UI_GRAPHICS_H
#define BRAMBLE_UI_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

int ui_graphics_init(void);
uint32_t ui_graphics_tick(void);
void ui_graphics_notify(uint32_t event_mask);
void ui_graphics_clear_unread(void);

/** Call from 1ms timer — feeds LVGL tick. */
void ui_graphics_tick_1ms(void);

#define UI_EVT_MSG_RECEIVED (1 << 0)
#define UI_EVT_NODE_UPDATE (1 << 1)
#define UI_EVT_STATS_UPDATE (1 << 2)
#define UI_EVT_BATTERY_UPDATE (1 << 3)
#define UI_EVT_GPS_UPDATE (1 << 4)
#define UI_EVT_CONN_CHANGE (1 << 5)
/* An already-stored outgoing message changed delivery status (an ACK or a
 * broadcast delivery receipt landed). Distinct from UI_EVT_MSG_RECEIVED: no new
 * message arrived, so there is nothing to mark unread, but an open thread is now
 * showing a stale badge and has to repaint. Without this the receipt lands, the
 * store says delivered, and the bubble keeps its pending dot until the user
 * leaves the thread or sends again. */
#define UI_EVT_MSG_STATUS (1 << 6)

/* Bench debug: remote screenshot (bramble.screenshot RPC).
 * LVGL is not thread-safe, so the capture itself runs on the UI task,
 * hooked into ui_graphics_tick() (see ui_graphics_request_screenshot's doc
 * below). The RPC handler (a transport task) only requests and waits. */

#define UI_SCREENSHOT_WIDTH 320
#define UI_SCREENSHOT_HEIGHT 240
#define UI_SCREENSHOT_LEN (UI_SCREENSHOT_WIDTH * UI_SCREENSHOT_HEIGHT * 2) /* RGB565 */

/**
 * Request a fresh LVGL screenshot. Sets a flag that ui_graphics_tick()
 * checks and services on the UI task (the only task allowed to touch LVGL),
 * then blocks the calling task on a semaphore until that completes or
 * timeout_ms elapses. Safe to call concurrently from multiple RPC transport
 * tasks (internally serialized).
 * Returns true on success (frame available via ui_graphics_get_screenshot),
 * false on timeout or capture failure.
 */
bool ui_graphics_request_screenshot(uint32_t timeout_ms);

/**
 * Returns a pointer to the last successfully captured frame (RGB565,
 * UI_SCREENSHOT_WIDTH x UI_SCREENSHOT_HEIGHT, *out_len bytes) or NULL if no
 * screenshot has ever been captured. The pointer is stable until the next
 * successful ui_graphics_request_screenshot() call.
 */
const uint8_t* ui_graphics_get_screenshot(size_t* out_len);

#endif
