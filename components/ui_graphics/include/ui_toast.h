#ifndef BRAMBLE_UI_TOAST_H
#define BRAMBLE_UI_TOAST_H

/* Transient on-screen feedback. Shows text bottom-center on the top layer
 * for ~2.5 s. Each call replaces any toast currently showing. Safe to call
 * from LVGL timer/event context only (same task as lv_timer_handler). */
void ui_toast_show(const char* text);

#endif
