#ifndef BRAMBLE_THEME_H
#define BRAMBLE_THEME_H

#include "lvgl.h"

/* Bramble color palette — dark theme */
#define BR_COLOR_BG          lv_color_hex(0x1A1A2E)
#define BR_COLOR_SURFACE     lv_color_hex(0x16213E)
#define BR_COLOR_PRIMARY     lv_color_hex(0x0F9B8E)
#define BR_COLOR_ACCENT      lv_color_hex(0xF0A500)
#define BR_COLOR_TEXT         lv_color_hex(0xEAEAEA)
#define BR_COLOR_TEXT_SEC     lv_color_hex(0x8892A0)
#define BR_COLOR_SENT         lv_color_hex(0x0D7377)
#define BR_COLOR_RECV         lv_color_hex(0x2C3E6B)
#define BR_COLOR_DANGER       lv_color_hex(0xE74C3C)
#define BR_COLOR_SUCCESS      lv_color_hex(0x2ECC71)

/* Standard dimensions */
#define BR_STATUS_BAR_H     20
#define BR_TAB_BAR_H        40
#define BR_COMPOSE_BAR_H    44
#define BR_CONTENT_H        (240 - BR_STATUS_BAR_H - BR_TAB_BAR_H)
#define BR_TAP_TARGET_MIN   40
#define BR_PADDING           8
#define BR_RADIUS            6

void bramble_theme_init(lv_display_t *disp);

#endif
