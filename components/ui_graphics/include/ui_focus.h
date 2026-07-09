#ifndef BRAMBLE_UI_FOCUS_H
#define BRAMBLE_UI_FOCUS_H

#include "lvgl.h"

/* Modal focus trap: while a modal is pushed, every keypad/encoder input
 * device drives a fresh group holding only the modal's widgets, so
 * keyboard/trackball navigation cannot reach controls hidden behind the
 * overlay. Single-level: pushing while pushed replaces the prior modal. */
void ui_focus_push_modal(void);
void ui_focus_pop_modal(void);

/* Group new focusable widgets should join: the modal group while one is
 * pushed, else the default group. */
lv_group_t* ui_focus_active_group(void);

#endif
