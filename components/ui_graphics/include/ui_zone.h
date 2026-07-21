#ifndef BRAMBLE_UI_ZONE_H
#define BRAMBLE_UI_ZONE_H

#include "lvgl.h"
#include "ui.h" /* ui_button_t */

/* Two-zone focus navigation for the T-Deck trackball.
 *
 * Every screen splits its focusable widgets into two LVGL groups:
 *   CONTENT - the working set (list rows, chat bubbles, compose+send, ...).
 *   CHROME  - the bottom nav tabs plus the screen's header actions
 *             (back button, channel-switch, "Msg" / "+ Ch", ...).
 *
 * The trackball is a KEYPAD indev whose read callback runs every button
 * through ui_zone_translate(), which maps the four directions onto the two
 * zones instead of a single flat next/prev walk:
 *   - UP/DOWN   walk vertically WITHIN the current zone (content history
 *               scrolls for free via LV_OBJ_FLAG_SCROLL_ON_FOCUS). From
 *               chrome, either vertical direction drops back to content.
 *   - LEFT/RIGHT walk the horizontal chrome. From content they HOP into
 *               chrome, except when the focused widget legitimately consumes
 *               horizontal keys (a textarea cursor, a slider, a dropdown).
 *   - SELECT    activates the focused widget (LV_KEY_ENTER), unchanged.
 * Zone boundaries never wrap and never switch screens by accident.
 *
 * The CONTENT group is registered as LVGL's default group, so screens keep
 * adding their working-set widgets with the existing lv_group_get_default()
 * calls; only chrome widgets move to ui_zone_chrome_group(). */

typedef enum {
    UI_ZONE_CONTENT = 0,
    UI_ZONE_CHROME,
} ui_zone_t;

/* Opt a content widget in to receiving raw UP/DOWN keys (LV_EVENT_KEY) instead
 * of group prev/next navigation, the vertical analogue of what a textarea gets
 * horizontally. The map canvas uses it for trackball zoom. LEFT/RIGHT still hop
 * to chrome, so a flagged widget can never trap focus. */
#define UI_ZONE_FLAG_CONSUMES_VERTICAL LV_OBJ_FLAG_USER_1

/* Create the content + chrome groups and register content as the default
 * group. Call once, before the trackball/keyboard indevs are created. */
void ui_zone_init(void);

lv_group_t* ui_zone_content_group(void);
lv_group_t* ui_zone_chrome_group(void);
ui_zone_t ui_zone_current(void);

/* Cache a widget pointer that outlives the statement that created it:
 *
 *     ui_zone_track(&s_msg_list, lv_obj_create(cont));
 *
 * Screens keep module-level lv_obj_t* handles (a message list, a compose box,
 * the chrome default) and rebuild by lv_obj_clean()ing the content area. The
 * clean frees those widgets, but a plain cached pointer still points at the
 * freed object, and the next lv_group_add_obj() / lv_obj_get_group() on it
 * reads freed memory: a LoadProhibited reboot. Remembering to null every cache
 * by hand at every clean site does not scale (missing one is exactly how the
 * SAS Back button rebooted the device).
 *
 * A tracked pointer is nulled by LVGL itself when its widget is deleted, so it
 * can never dangle: the cache is live if and only if the widget is. Tracking
 * the same slot again just retargets it (no callback pile-up), and a stale
 * widget's delete never clears a slot that has since moved on. */
void ui_zone_track(lv_obj_t** slot, lv_obj_t* obj);

/* The chrome widget a content->chrome hop should land on first (typically the
 * active bottom-nav tab). Ignored once the chrome group already holds focus.
 * Tracked, so a header action that dies with its content area does not leave
 * the hop target dangling. */
void ui_zone_set_chrome_default(lv_obj_t* obj);

/* Apply the chrome focus indicator (a bright accent BORDER on LV_STATE_FOCUSED)
 * to a chrome widget. Distinct in colour from the content zone's fill
 * highlight, so which zone holds the live cursor is unambiguous. Call it on
 * every tab / header-action button added to the chrome group. Borders, never
 * outlines: an outline draws outside the widget and clips at unpadded
 * container edges (see ui_zone_style_chrome's comment). */
void ui_zone_style_chrome(lv_obj_t* obj);

/* Apply the content zone's focus indicator (a primary-colour border on
 * LV_STATE_FOCUSED) to a content widget, e.g. a chat bubble. */
void ui_zone_style_content(lv_obj_t* obj);

/* Add obj to the chrome group, style it, and (if make_default) make it the
 * widget a content->chrome hop lands on. The one-stop version of the
 * add+style(+default) stanza every screen's header/tab builder repeats. */
void ui_zone_add_chrome(lv_obj_t* obj, bool make_default);

/* Bind every keypad/encoder indev to a group. Shared by the zone-hop path and
 * ui_focus's modal group swap. */
void ui_zone_bind_indevs(lv_group_t* g);

/* A full-size, vertically scrollable column to hang a screen's content on.
 *
 * The layout's content_area is a FIXED canvas: layout_create clears
 * LV_OBJ_FLAG_SCROLLABLE on it so a child list reaching its scroll end cannot
 * drag pinned elements (a chat compose bar, a header) off screen. A screen that
 * therefore parents its rows straight onto content_area gets a column with no
 * scrollable ancestor at all, and the content group's scroll-on-focus below has
 * nothing to scroll: focus walks past the fold, the cursor disappears, and
 * SELECT fires an invisible widget. Screens whose content is one long column
 * (settings, stats) build it in here instead. */
lv_obj_t* ui_zone_scroll_column(lv_obj_t* parent);

/* Bind every keypad/encoder indev to a zone's group and make it current.
 * A hop into an empty zone is refused (focus is never stranded). */
void ui_zone_activate(ui_zone_t zone);

/* Bind input back to the content zone unconditionally and mark it current.
 * Screens call this when they (re)build their content, and ui_focus calls it
 * when a modal closes, so focus always starts in a known place. */
void ui_zone_reset_to_content(void);

/* Map one trackball button to the LVGL key the read callback should emit
 * (0 = none: a pure zone hop was performed as a side effect). */
uint32_t ui_zone_translate(ui_button_t btn);

/* Run a screen transition AFTER the current LVGL event finishes dispatching.
 *
 * Every screen builder here rebuilds by lv_obj_clean()ing the content area (or
 * the message list) and repopulating it. When the thing that triggers the
 * rebuild is a CLICKED handler on a button that lives INSIDE that container -
 * a Back button, a list row, the SAS "Codes Match" button, a chat bubble - the
 * clean deletes the very widget whose event is still being dispatched. LVGL 9
 * forbids that ("you cannot delete an object in its own event handler").
 *
 * Deferring is necessary but not sufficient: the widgets are still deleted,
 * only later, so anything CACHING one of them must use ui_zone_track.
 *
 * So a click handler never rebuilds inline: it stashes what it needs and hands
 * the transition to ui_defer(), which runs it from lv_async_call on the next
 * LVGL tick, once event dispatch has unwound and the widget can safely die.
 *
 * Handlers whose widget is NOT a descendant of what gets cleaned (the bottom
 * nav tabs live in the tab bar, not the content area) may still rebuild inline. */
void ui_defer(lv_async_cb_t cb, void* arg);

/* Register a CLICKED handler that always runs cb through ui_defer (after LVGL
 * finishes dispatching the event), so a handler that rebuilds/cleans the screen
 * can never delete its own widget mid-dispatch. THE way to register any click
 * that triggers a screen transition or rebuild: it collapses the hand-rolled
 * "static foo_async(void*) + static foo_click_cb(lv_event_t*) that just
 * ui_defer's it" pair into a single line. cb receives ctx, exactly what the
 * hand-rolled ui_defer(foo_async, ctx) would have passed.
 *
 * A click that must do real work BEFORE the transition (read a textarea that the
 * rebuild destroys, toggle a selection, snapshot state a later rebuild would
 * recycle) keeps a custom handler that does that work synchronously and ends in
 * ui_defer; this helper is for the pure "defer this transition" case. */
void ui_zone_add_deferred_click(lv_obj_t* obj, lv_async_cb_t cb, void* ctx);

#endif /* BRAMBLE_UI_ZONE_H */
