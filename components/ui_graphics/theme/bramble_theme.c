#include "bramble_theme.h"
/* Full lv_theme_t definition: LVGL 9 keeps the struct private, and deriving
 * a theme (copy the default, swap the apply callback) is the documented use
 * of this header. */
#include "themes/lv_theme_private.h"

/* Derived theme: the LVGL default theme plus one Bramble-wide rule.
 *
 * The rule: NO focus outlines, anywhere. LVGL outlines draw OUTSIDE the
 * widget's bounds, so a focused widget flush against an unpadded parent edge
 * gets its ring clipped (node-detail's top action button lost its top edge,
 * the chrome tabs and the map viewport lost their bottom edge at the screen
 * boundary), and scroll-to-view only considers the widget itself, so no
 * amount of scrolling can ever reveal the missing side. Focus indication is
 * a BORDER instead (ui_zone_style_chrome/content), which draws inside the
 * bounds and therefore can never clip. The default theme hangs its own
 * outline on LV_STATE_FOCUS_KEY; this apply callback zeroes it on every
 * widget the theme touches so the banished ring cannot leak back in. */
static lv_theme_t s_theme;

static void theme_apply_cb(lv_theme_t* th, lv_obj_t* obj) {
    (void)th;
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    /* REPLACE the banished outline, don't just delete it: widgets outside the
     * two zone groups (modal buttons, textareas in dialogs) had the theme
     * outline as their ONLY keypad-focus indicator, and removing it left
     * focus invisible on the New Channel dialog. A theme-level border on
     * FOCUS_KEY restores it inside the widget bounds. Zone-styled widgets
     * override this with their LOCAL border on FOCUSED (local styles win
     * over theme styles), so chrome keeps its accent blue. */
    lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(obj, BR_COLOR_PRIMARY, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
}

void bramble_theme_init(lv_display_t* disp) {
    /* Initialize LVGL's default dark theme, then derive from it */
    lv_theme_t* def =
        lv_theme_default_init(disp, BR_COLOR_PRIMARY, BR_COLOR_ACCENT, true, /* dark mode */
                              &lv_font_montserrat_14);
    s_theme = *def;
    lv_theme_set_parent(&s_theme, def);
    lv_theme_set_apply_cb(&s_theme, theme_apply_cb);
    lv_display_set_theme(disp, &s_theme);

    /* Set the screen background color directly */
    lv_obj_set_style_bg_color(lv_screen_active(), BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_screen_active(), BR_COLOR_TEXT, 0);
}
