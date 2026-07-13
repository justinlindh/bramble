#ifndef BRAMBLE_THEME_H
#define BRAMBLE_THEME_H

#include "lvgl.h"

/* Bramble color palette — dark theme (matches webapp) */
#define BR_COLOR_BG lv_color_hex(0x0D1117)        /* --bg */
#define BR_COLOR_SURFACE lv_color_hex(0x161B22)   /* --surface */
#define BR_COLOR_SURFACE_2 lv_color_hex(0x21262D) /* --surface-2 */
#define BR_COLOR_BORDER lv_color_hex(0x30363D)    /* --border */
#define BR_COLOR_PRIMARY lv_color_hex(0x238636)   /* --accent (green) */
#define BR_COLOR_ACCENT lv_color_hex(0x1F6FEB)    /* --accent-blue */
#define BR_COLOR_TEXT lv_color_hex(0xE6EDF3)      /* --text */
#define BR_COLOR_TEXT_SEC lv_color_hex(0x8B949E)  /* --text-muted */
/* Outgoing bubbles are filled with BR_COLOR_SENT, so anything drawn ON one must
 * not be that same colour or it is invisible. The delivered badge used to be
 * BR_COLOR_PRIMARY, which IS BR_COLOR_SENT: delivery confirmations rendered
 * perfectly and could never be seen. Use BR_COLOR_ON_SENT for marks on an
 * outgoing bubble, and let the compiler refuse the collision. */
#define BR_HEX_SENT 0x238636
#define BR_HEX_ON_SENT 0xE6EDF3
#define BR_COLOR_SENT lv_color_hex(BR_HEX_SENT) /* matches primary accent */
#define BR_COLOR_ON_SENT lv_color_hex(BR_HEX_ON_SENT)
_Static_assert(BR_HEX_ON_SENT != BR_HEX_SENT,
               "a mark on an outgoing bubble must contrast with BR_COLOR_SENT, "
               "otherwise it renders invisible (this shipped once)");
#define BR_COLOR_RECV lv_color_hex(0x21262D)     /* surface-2 for incoming */
#define BR_COLOR_DANGER lv_color_hex(0xDA3633)   /* --danger */
#define BR_COLOR_SUCCESS lv_color_hex(0x238636)  /* --accent (green) */
#define BR_COLOR_WARNING lv_color_hex(0xE3B341)  /* --warning */
#define BR_COLOR_CRITICAL lv_color_hex(0xBC8CFF) /* --critical */

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
