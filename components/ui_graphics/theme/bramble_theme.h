#ifndef BRAMBLE_THEME_H
#define BRAMBLE_THEME_H

#include "lvgl.h"

/* Bramble color palette: dark theme (matches webapp) */
#define BR_COLOR_BG lv_color_hex(0x0D1117)        /* --bg */
#define BR_COLOR_SURFACE lv_color_hex(0x161B22)   /* --surface */
#define BR_COLOR_SURFACE_2 lv_color_hex(0x21262D) /* --surface-2 */
#define BR_COLOR_BORDER lv_color_hex(0x30363D)    /* --border */
#define BR_HEX_PRIMARY 0x238636                   /* --accent (green) */
#define BR_COLOR_PRIMARY lv_color_hex(BR_HEX_PRIMARY)
#define BR_COLOR_ACCENT lv_color_hex(0x1F6FEB)   /* --accent-blue */
#define BR_COLOR_TEXT lv_color_hex(0xE6EDF3)     /* --text */
#define BR_COLOR_TEXT_SEC lv_color_hex(0x8B949E) /* --text-muted */
/* Outgoing bubbles fill with BR_COLOR_SENT, so any mark drawn ON one must
 * contrast with that fill or it renders invisible. SENT is its own role: a
 * dark blue, deliberately NOT the brand/success green, which already carries
 * brand, action, success and focus. Keeping the roles apart lets the delivered
 * double-check be success-green and still read clearly on the blue bubble.
 * BR_COLOR_ON_SENT is the light text for muted marks (the age) on an outgoing
 * bubble. The static-asserts below refuse any palette edit that would reunify
 * these roles. */
#define BR_HEX_SENT 0x1A4B91
#define BR_HEX_ON_SENT 0xE6EDF3
#define BR_COLOR_SENT lv_color_hex(BR_HEX_SENT) /* outgoing bubble fill (blue) */
#define BR_COLOR_ON_SENT lv_color_hex(BR_HEX_ON_SENT)
#define BR_COLOR_RECV lv_color_hex(0x21262D)   /* surface-2 for incoming */
#define BR_COLOR_DANGER lv_color_hex(0xDA3633) /* --danger */
#define BR_HEX_SUCCESS 0x238636                /* --accent (green) */
#define BR_COLOR_SUCCESS lv_color_hex(BR_HEX_SUCCESS)
#define BR_COLOR_WARNING lv_color_hex(0xE3B341)  /* --warning */
#define BR_COLOR_CRITICAL lv_color_hex(0xBC8CFF) /* --critical */

_Static_assert(BR_HEX_ON_SENT != BR_HEX_SENT,
               "a mark on an outgoing bubble must contrast with BR_COLOR_SENT, "
               "otherwise it renders invisible (this shipped once)");
_Static_assert(BR_HEX_SENT != BR_HEX_PRIMARY,
               "the outgoing bubble must not share the brand/action green, or "
               "the green-on-green invisibility bug returns");
_Static_assert(BR_HEX_SENT != BR_HEX_SUCCESS,
               "the delivered badge is success-green drawn on the outgoing "
               "bubble, so SENT must differ from SUCCESS or it goes invisible");

/* Standard dimensions */
#define BR_STATUS_BAR_H 20
#define BR_TAB_BAR_H 40
#define BR_COMPOSE_BAR_H 44
#define BR_CONTENT_H (240 - BR_STATUS_BAR_H - BR_TAB_BAR_H)
#define BR_TAP_TARGET_MIN 40
#define BR_PADDING 8
#define BR_RADIUS 6

void bramble_theme_init(lv_display_t* disp);

#endif
