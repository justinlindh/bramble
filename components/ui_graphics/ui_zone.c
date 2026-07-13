#include "ui_zone.h"
#include "ui_focus.h"
#include "theme/bramble_theme.h"
#include "esp_log.h"

static const char* TAG = "ui_zone";

static lv_group_t* s_content = NULL;
static lv_group_t* s_chrome = NULL;
static ui_zone_t s_zone = UI_ZONE_CONTENT;
static lv_obj_t* s_chrome_default = NULL;

void ui_zone_bind_indevs(lv_group_t* g) {
    if (!g)
        return;
    for (lv_indev_t* indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER)
            lv_indev_set_group(indev, g);
    }
}

/* Scroll whatever just gained focus into view. Registered on the content
 * group so vertical navigation walks (and scrolls) long lists - chat history,
 * node peers, the settings column - without every screen tagging each widget
 * with LV_OBJ_FLAG_SCROLL_ON_FOCUS. */
static void zone_focus_changed(lv_group_t* g);

static void content_focus_cb(lv_group_t* g) {
    lv_obj_t* foc = lv_group_get_focused(g);
    if (foc) {
        /* No animation: the chat render path re-asserts scroll position right
         * after refocusing, and a pending focus-scroll animation would fight
         * that. Instant scroll-into-view is fine on a 320x240 panel. */
        lv_obj_scroll_to_view_recursive(foc, LV_ANIM_OFF);
        /* Nothing scrolled it in: the widget has no scrollable ancestor (see
         * ui_zone_scroll_column). Focus is now on something the user cannot
         * see, and SELECT would fire it blind. Only a builder bug can cause
         * this, so shout rather than paper over it. */
        if (!lv_obj_is_visible(foc))
            ESP_LOGE(TAG, "focused widget is off screen: its content container does not scroll");
    }
    zone_focus_changed(g);
}

static void chrome_focus_cb(lv_group_t* g) { zone_focus_changed(g); }

/* See ui_zone.h: a click handler must never delete its own widget's ancestor
 * inline. lv_async_call runs cb at the top of the next lv_timer_handler, after
 * event dispatch has unwound. */
void ui_defer(lv_async_cb_t cb, void* arg) {
    if (!cb)
        return;
    if (lv_async_call(cb, arg) != LV_RESULT_OK) {
        /* Only fails if LVGL cannot allocate the async node. Dropping the
         * transition strands the user on the current screen, which is
         * recoverable; running it inline would reboot the device. */
        ESP_LOGE(TAG, "lv_async_call failed; screen transition dropped");
    }
}

/* See ui_zone.h: LVGL nulls the cache when it deletes the widget. Guarded on
 * identity because a slot can be retargeted (the chrome default follows the
 * active tab), and the widget it used to hold must not clear it on its way
 * out. */
static void untrack_cb(lv_event_t* e) {
    lv_obj_t** slot = (lv_obj_t**)lv_event_get_user_data(e);
    if (slot && *slot == lv_event_get_target_obj(e))
        *slot = NULL;
}

void ui_zone_track(lv_obj_t** slot, lv_obj_t* obj) {
    if (!slot)
        return;
    *slot = obj;
    if (!obj)
        return;
    /* Idempotent: the chrome default retracks the same long-lived tab button on
     * every tab switch, which would otherwise pile up delete callbacks on it. */
    lv_obj_remove_event_cb_with_user_data(obj, untrack_cb, slot);
    lv_obj_add_event_cb(obj, untrack_cb, LV_EVENT_DELETE, slot);
}

void ui_zone_init(void) {
    s_content = lv_group_create();
    s_chrome = lv_group_create();
    /* Deliberate, non-wrapping boundaries: hitting the end of a zone stops
     * rather than silently looping or slipping into the other zone. */
    lv_group_set_wrap(s_content, false);
    lv_group_set_wrap(s_chrome, false);
    lv_group_set_focus_cb(s_content, content_focus_cb);
    lv_group_set_focus_cb(s_chrome, chrome_focus_cb);
    lv_group_set_default(s_content);
    s_zone = UI_ZONE_CONTENT;
    s_chrome_default = NULL;
}

lv_group_t* ui_zone_content_group(void) { return s_content; }
lv_group_t* ui_zone_chrome_group(void) { return s_chrome; }
ui_zone_t ui_zone_current(void) { return s_zone; }
void ui_zone_set_chrome_default(lv_obj_t* obj) { ui_zone_track(&s_chrome_default, obj); }

void ui_zone_style_chrome(lv_obj_t* obj) {
    if (!obj)
        return;
    /* Accent (blue) outline, distinct from the green fill the content zone uses
     * for its focused row, so a chrome hop is visible even on the active tab
     * that already carries a green background. */
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, BR_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 1, LV_STATE_FOCUSED);
}

void ui_zone_style_content(lv_obj_t* obj) {
    if (!obj)
        return;
    /* Primary-colour outline, distinct from the chrome accent. No explicit
     * opa: content widgets (chat bubbles) never wanted the forced-opaque
     * outline the chrome style uses. */
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 1, LV_STATE_FOCUSED);
}

void ui_zone_add_chrome(lv_obj_t* obj, bool make_default) {
    /* s_chrome is non-NULL after ui_zone_init (called at splash, before any
     * screen builder runs), but keep the guard here since this is now the
     * only place that adds to the chrome group. */
    if (s_chrome)
        lv_group_add_obj(s_chrome, obj);
    ui_zone_style_chrome(obj);
    if (make_default)
        ui_zone_set_chrome_default(obj);
}

lv_obj_t* ui_zone_scroll_column(lv_obj_t* parent) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    /* Vertical only: rows are sized to the panel width, and a stray horizontal
     * scroll would just smear them sideways. */
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    return col;
}

static bool group_empty(lv_group_t* g) { return !g || lv_group_get_obj_count(g) == 0; }

/* The focus states LVGL itself puts on (and takes off) a widget that a
 * keypad/encoder group focuses. lv_obj.c adds FOCUSED|FOCUS_KEY on
 * LV_EVENT_FOCUSED from a keypad indev, and clears FOCUSED|EDITED|FOCUS_KEY on
 * LV_EVENT_DEFOCUSED. Both bits matter: the widget's own highlight (our green
 * content fill / accent chrome outline) hangs off LV_STATE_FOCUSED, but the
 * DEFAULT THEME's focus outline hangs off LV_STATE_FOCUS_KEY. Clearing only
 * FOCUSED therefore left the theme outline burning on the inactive zone, so a
 * content row and a chrome tab were lit at the same time and you could not tell
 * which zone the trackball was driving. Mirror LVGL's own defocus set instead. */
#define UI_ZONE_FOCUS_STATES (LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY)

/* Exactly one visible cursor: light the live zone's focused widget and fully
 * defocus the other zone's. The stale object keeps its place in its group (only
 * the visual state is cleared), so returning to that zone lights it again. */
static void apply_focus_visual(lv_group_t* active) {
    lv_group_t* other = (active == s_chrome) ? s_content : s_chrome;
    lv_obj_t* off = other ? lv_group_get_focused(other) : NULL;
    if (off) {
        lv_obj_remove_state(off, UI_ZONE_FOCUS_STATES | LV_STATE_EDITED);
        lv_obj_invalidate(off);
    }
    lv_obj_t* on = active ? lv_group_get_focused(active) : NULL;
    if (on) {
        lv_obj_add_state(on, UI_ZONE_FOCUS_STATES);
        lv_obj_invalidate(on);
    }
}

/* Focus moved WITHIN a group: LVGL lights the newly focused widget, but it
 * never touches the other group, so re-assert the one-cursor invariant. */
static void zone_focus_changed(lv_group_t* g) {
    lv_group_t* live = (s_zone == UI_ZONE_CHROME) ? s_chrome : s_content;
    if (g == live)
        apply_focus_visual(live);
}

void ui_zone_activate(ui_zone_t zone) {
    lv_group_t* g = (zone == UI_ZONE_CHROME) ? s_chrome : s_content;
    if (group_empty(g))
        return; /* never strand focus in an empty zone */

    s_zone = zone;
    ui_zone_bind_indevs(g);

    bool focused_fresh = false;
    if (zone == UI_ZONE_CHROME && !lv_group_get_focused(g)) {
        /* Fresh entry: land on the active tab if it is still a chrome member,
         * else the first chrome widget. lv_group_focus_obj fires the chrome
         * focus_cb synchronously, which already re-applies the one-cursor
         * visual below, so skip the redundant second call. */
        if (s_chrome_default && lv_obj_get_group(s_chrome_default) == g)
            lv_group_focus_obj(s_chrome_default);
        else
            lv_group_focus_obj(lv_group_get_obj_by_index(g, 0));
        focused_fresh = true;
    }

    if (!focused_fresh)
        apply_focus_visual(g);
}

void ui_zone_reset_to_content(void) {
    /* A screen with no focusable content (map, traffic) starts in chrome so
     * focus lands somewhere visible instead of an empty group. */
    if (group_empty(s_content) && !group_empty(s_chrome)) {
        ui_zone_activate(UI_ZONE_CHROME);
        return;
    }
    s_zone = UI_ZONE_CONTENT;
    ui_zone_bind_indevs(s_content);
    apply_focus_visual(s_content);
}

/* Focused object of whatever group input is bound to right now. Reads the
 * live indev binding so it stays correct even while a modal (ui_focus) owns a
 * group of its own, not one of the two zones. */
static lv_obj_t* active_focused(void) {
    for (lv_indev_t* indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER) {
            lv_group_t* g = lv_indev_get_group(indev);
            return g ? lv_group_get_focused(g) : NULL;
        }
    }
    return NULL;
}

/* Is the content zone's focused widget the first one in the ring (nothing above
 * it to walk up to)? */
static bool at_top_of_content(void) {
    if (group_empty(s_content))
        return false;
    lv_obj_t* foc = lv_group_get_focused(s_content);
    return foc && foc == lv_group_get_obj_by_index(s_content, 0);
}

/* Widgets that own the horizontal axis for their own editing: LEFT/RIGHT
 * moves a textarea cursor, nudges a slider, or steps a dropdown/roller
 * selection instead of hopping to chrome. */
static bool consumes_horizontal(lv_obj_t* obj) {
    if (!obj)
        return false;
    return lv_obj_check_type(obj, &lv_textarea_class) || lv_obj_check_type(obj, &lv_slider_class) ||
           lv_obj_check_type(obj, &lv_dropdown_class) || lv_obj_check_type(obj, &lv_roller_class);
}

uint32_t ui_zone_translate(ui_button_t btn) {
    lv_obj_t* foc = active_focused();

    if (btn == BTN_SELECT)
        return LV_KEY_ENTER;

    /* An open dropdown consumes every arrow to move through its option list;
     * navigating the group out from under it would just close it. */
    if (foc && lv_obj_check_type(foc, &lv_dropdown_class) && lv_dropdown_is_open(foc)) {
        switch (btn) {
        case BTN_UP:
            return LV_KEY_UP;
        case BTN_DOWN:
            return LV_KEY_DOWN;
        case BTN_LEFT:
            return LV_KEY_LEFT;
        case BTN_RIGHT:
            return LV_KEY_RIGHT;
        default:
            return 0;
        }
    }

    /* Modal (ui_focus) trap: one flat group, no zones. UP/DOWN navigate;
     * LEFT/RIGHT navigate too unless the focused widget wants the cursor. */
    if (ui_focus_modal_active()) {
        switch (btn) {
        case BTN_UP:
            return LV_KEY_PREV;
        case BTN_DOWN:
            return LV_KEY_NEXT;
        case BTN_LEFT:
            return consumes_horizontal(foc) ? LV_KEY_LEFT : LV_KEY_PREV;
        case BTN_RIGHT:
            return consumes_horizontal(foc) ? LV_KEY_RIGHT : LV_KEY_NEXT;
        default:
            return 0;
        }
    }

    if (s_zone == UI_ZONE_CONTENT) {
        switch (btn) {
        case BTN_UP:
            /* UP off the TOP of the content zone escapes to chrome (landing on
             * the screen's first header action: Back / Msg). Without this, UP at
             * the top of content is a dead key, and an empty chat traps the
             * user: the auto-focused compose box is then the ONLY content
             * widget, it swallows LEFT/RIGHT for its own cursor, and with no
             * bubble above it there was no obvious route to Back at all.
             * Non-empty lists are unaffected: UP still walks up the rows, and
             * only escapes once there is nothing above. */
            if (at_top_of_content()) {
                ui_zone_activate(UI_ZONE_CHROME);
                return 0;
            }
            return LV_KEY_PREV;
        case BTN_DOWN:
            return LV_KEY_NEXT;
        case BTN_LEFT:
            if (consumes_horizontal(foc))
                return LV_KEY_LEFT;
            ui_zone_activate(UI_ZONE_CHROME);
            return 0;
        case BTN_RIGHT:
            if (consumes_horizontal(foc))
                return LV_KEY_RIGHT;
            ui_zone_activate(UI_ZONE_CHROME);
            return 0;
        default:
            return 0;
        }
    }

    /* CHROME: horizontal walks the strip, vertical drops back to content. */
    switch (btn) {
    case BTN_UP:
    case BTN_DOWN:
        ui_zone_activate(UI_ZONE_CONTENT);
        return 0;
    case BTN_LEFT:
        return LV_KEY_PREV;
    case BTN_RIGHT:
        return LV_KEY_NEXT;
    default:
        return 0;
    }
}
