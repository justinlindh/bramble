# T-Deck Liveness/Input Batch (B4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keyboard/trackball users can actually navigate (tabs reachable, modals trap focus), and the data screens stop being frozen snapshots (live nodes, live map, live traffic monitor with its missing Settings toggle, honest map counts, dead-card fix).

**Architecture:** A tiny `ui_focus` helper owns a modal input group (push/pop switches every keypad/encoder indev). `scr_layout` gains a tab-bar hide/show helper that also removes/re-adds tab buttons from the focus group. Per-screen `lv_timer`s (deleted via `LV_EVENT_DELETE` on a screen-owned widget) do in-place updates on Nodes and rebuilds on Map/Traffic. Stats stays a snapshot deliberately (its deltas are defined as "since last visit").

**Tech Stack:** ESP-IDF C, LVGL v9. No host-testable logic beyond what exists; gate is the tdeck build + host suite staying green.

## Global Constraints

- No em dashes; no AI attribution; conventional commits (house rules).
- Branch: `feat/tdeck-liveness` off `origin/main` (already created).
- Gates: `bash scripts/flash.sh local tdeck-plus build`, `bash test/run_all_tests.sh`.
- Timer lifecycle rule: every screen-refresh `lv_timer` is deleted from an `LV_EVENT_DELETE` callback registered on a widget that dies with the screen (`layout_set_tab` cleans the content area), so no timer ever touches deleted widgets.
- This batch is "somewhat complicated": run /code-review on the diff and fix findings BEFORE merging (standing user instruction).

---

### Task 1: Tabs reachable by keyboard/trackball

**Files:**
- Modify: `components/ui_graphics/screens/scr_layout.c` (`layout_create`; new helper)
- Modify: `components/ui_graphics/screens/scr_layout.h` (declare helper)
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (2 hide/show sites)
- Modify: `components/ui_graphics/screens/scr_chat_compose.c` (2 sites)
- Modify: `components/ui_graphics/screens/scr_channel_create.c` (2 sites)

- [ ] **Step 1:** In `layout_create`, inside the tab-button loop after `s_layout.tab_btns[i] = btn;`, add:

```c
        lv_group_t* g = lv_group_get_default();
        if (g)
            lv_group_add_obj(g, btn);
```

- [ ] **Step 2:** Add to `scr_layout.h` (next to `layout_set_unread`):

```c
/* Hide/show the tab bar AND remove/re-add its buttons from the input
 * group, so hidden tabs cannot be focused by keyboard/trackball. */
void layout_set_tab_bar_hidden(bramble_layout_t* layout, bool hidden);
```

Implementation in `scr_layout.c`:

```c
void layout_set_tab_bar_hidden(bramble_layout_t* layout, bool hidden) {
    lv_group_t* g = lv_group_get_default();
    if (hidden) {
        lv_obj_add_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < TAB_COUNT; i++) {
            if (layout->tab_btns[i])
                lv_group_remove_obj(layout->tab_btns[i]);
        }
    } else {
        lv_obj_clear_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < TAB_COUNT; i++) {
            if (g && layout->tab_btns[i])
                lv_group_add_obj(g, layout->tab_btns[i]);
        }
    }
}
```

- [ ] **Step 3:** Replace the direct flag flips:
- `scr_chat_messages.c`: `lv_obj_add_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);` in `open_with_target` and `lv_obj_clear_flag(...)` in `back_click_cb` become `layout_set_tab_bar_hidden(layout, true/false);`
- `scr_chat_compose.c`: same two sites.
- `scr_channel_create.c`: same two sites (open hides, cancel restores).

- [ ] **Step 4:** Build gate + commit `feat(ui_gfx): tabs reachable by keyboard and trackball`.

---

### Task 2: Modal focus trap (`ui_focus`)

**Files:**
- Create: `components/ui_graphics/include/ui_focus.h`, `components/ui_graphics/ui_focus.c`
- Modify: `components/ui_graphics/CMakeLists.txt` (add `"ui_focus.c"`)
- Modify: `components/ui_graphics/ui_confirm.c`, `components/ui_graphics/screens/scr_settings.c` (name edit, channel add, QR overlay)

**Interfaces:**

```c
void ui_focus_push_modal(void);        /* keypad+encoder indevs -> fresh modal group */
void ui_focus_pop_modal(void);         /* restore default group; delete modal group */
lv_group_t* ui_focus_active_group(void); /* modal group when pushed, else default */
```

- [ ] **Step 1:** Implement (`ui_focus.c`):

```c
#include "ui_focus.h"
#include "lvgl.h"

static lv_group_t* s_modal_group = NULL;

static void set_input_group(lv_group_t* g) {
    for (lv_indev_t* indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER)
            lv_indev_set_group(indev, g);
    }
}

void ui_focus_push_modal(void) {
    if (s_modal_group)
        ui_focus_pop_modal(); /* single-level: a new modal replaces the old */
    s_modal_group = lv_group_create();
    set_input_group(s_modal_group);
}

void ui_focus_pop_modal(void) {
    if (!s_modal_group)
        return;
    set_input_group(lv_group_get_default());
    lv_group_delete(s_modal_group);
    s_modal_group = NULL;
}

lv_group_t* ui_focus_active_group(void) {
    return s_modal_group ? s_modal_group : lv_group_get_default();
}
```

Header mirrors the three declarations with a comment that modals are single-level.

- [ ] **Step 2:** `ui_confirm.c`: call `ui_focus_push_modal()` at the top of `ui_confirm_show` (after `confirm_close()`), `ui_focus_pop_modal()` inside `confirm_close()` (before deleting), and use `ui_focus_active_group()` instead of `lv_group_get_default()` for the two buttons.

- [ ] **Step 3:** `scr_settings.c`: in `name_edit_cb` and `channel_add_open_cb`, call `ui_focus_push_modal()` first; in `name_edit_close` and `channel_add_close`, call `ui_focus_pop_modal()`; replace their `lv_group_get_default()` group lookups with `ui_focus_active_group()`. In `identity_qr_open_cb` push, in `identity_qr_close` pop, and add the QR close button to `ui_focus_active_group()` (find the Back/close button below the QR widget and group it).

- [ ] **Step 4:** Build gate + commit `fix(ui_gfx): modals trap keyboard/trackball focus`.

---

### Task 3: Live Nodes screen (+ dead cards, age text)

**Files:**
- Modify: `components/ui_graphics/screens/scr_nodes.c`

- [ ] **Step 1:** Raise the pool and show ages. `#define MAX_CARD_CTX 16` becomes `#define MAX_CARD_CTX MAX_NEIGHBORS` (routing.h defines 32; `ui_shared_state.h` already includes routing types). In `create_node_card` replace the info-line block (drop the `(void)age_s;`):

```c
    char info[48];
    uint32_t age_s = (now_ms - n->last_heard) / 1000;
    char age_buf[12];
    if (age_s < 60)
        snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
    else if (age_s < 3600)
        snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
    else
        snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
    snprintf(info, sizeof(info), "%ddBm  SNR:%d  %s", n->rssi, n->snr, age_buf);
```

- [ ] **Step 2:** In-place refresh. Extend the ctx and remember widget handles:

```c
typedef struct {
    bramble_layout_t* layout;
    neighbor_entry_t neighbor;
    uint32_t now_ms;
    lv_obj_t* info_lbl;
    lv_obj_t* bar;
    lv_obj_t* dot;
} node_card_ctx_t;
```

In `create_node_card`, after creating `info_lbl`, `bar`, `dot`, store them into the ctx used for the click handler (restructure so the ctx pointer is taken before widget creation and handles assigned after; cards beyond the pool still render but get no ctx, no click handler, and no live update, which the pool raise makes unreachable in practice).

Add a refresh timer in `scr_nodes_create` (after the list is built, only when `count > 0`):

```c
static void nodes_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    const ui_mesh_state_t* state = ui_shared_mesh_state();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    for (int c = 0; c < s_card_ctx_count; c++) {
        node_card_ctx_t* ctx = &s_card_ctx[c];
        if (!ctx->info_lbl)
            continue;
        for (int i = 0; i < state->neighbors.count; i++) {
            const neighbor_entry_t* n = &state->neighbors.entries[i];
            if (n->addr != ctx->neighbor.addr)
                continue;
            ctx->neighbor = *n;
            ctx->now_ms = now_ms;
            uint32_t age_s = (now_ms - n->last_heard) / 1000;
            char age_buf[12];
            if (age_s < 60)
                snprintf(age_buf, sizeof(age_buf), "%lus", (unsigned long)age_s);
            else if (age_s < 3600)
                snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(age_s / 60));
            else
                snprintf(age_buf, sizeof(age_buf), "%luh", (unsigned long)(age_s / 3600));
            lv_label_set_text_fmt(ctx->info_lbl, "%ddBm  SNR:%d  %s", n->rssi, n->snr, age_buf);
            int pct = (n->rssi + 120) * 100 / 70;
            if (pct < 0)
                pct = 0;
            if (pct > 100)
                pct = 100;
            lv_bar_set_value(ctx->bar, pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(ctx->dot,
                                      (age_s < 600) ? BR_COLOR_SUCCESS : BR_COLOR_TEXT_SEC, 0);
            break;
        }
    }
}

static void nodes_list_delete_cb(lv_event_t* e) {
    lv_timer_t* timer = (lv_timer_t*)lv_event_get_user_data(e);
    if (timer)
        lv_timer_delete(timer);
    s_card_ctx_count = 0;
}
```

Wire-up in `scr_nodes_create` after the card loop:

```c
    lv_timer_t* refresh = lv_timer_create(nodes_refresh_cb, 3000, NULL);
    lv_obj_add_event_cb(list, nodes_list_delete_cb, LV_EVENT_DELETE, refresh);
```

(Also zero the new handle fields where the pool is reset: `memset(s_card_ctx, 0, sizeof(s_card_ctx));` next to `s_card_ctx_count = 0;`.)

- [ ] **Step 3:** Build gate + commit `feat(ui_gfx): live nodes list with ages; fix dead cards past 16 peers`.

---

### Task 4: Map: live redraw, cleared focus, honest count

**Files:**
- Modify: `components/ui_graphics/screens/scr_map.c`
- Modify: `components/ui_graphics/screens/scr_map.h` (declare `scr_map_clear_focus_peer`)
- Modify: `components/ui_graphics/screens/scr_layout.c` (`tab_click_cb`)

- [ ] **Step 1:** Focus lifecycle. Add to `scr_map.c`/`.h`:

```c
void scr_map_clear_focus_peer(void) { s_focus_peer_addr = 0; }
```

In `scr_layout.c` `tab_click_cb`, before `layout_set_tab`, clear the sticky focus when the user taps the Map tab directly:

```c
    if (tab == TAB_MAP)
        scr_map_clear_focus_peer();
```

(`scr_map.h` is already included by `scr_layout.c`.)

- [ ] **Step 2:** Honest count. Make `create_marker` return `bool` (false when the bounds check drops the marker), count drawn markers in the peer loop, and render the badge as `N shown` plus ` (+M off-map)` when any were dropped.

- [ ] **Step 3:** Live redraw. `scr_map_create` stores the layout pointer in a static; add:

```c
static void map_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    if (!s_map_layout)
        return;
    lv_obj_t* cont = layout_get_content(s_map_layout);
    lv_refr_now(lv_display_get_default());
    lv_obj_clean(cont);
    scr_map_create(s_map_layout);
}
```

Register a 5 s timer at the end of `scr_map_create`, deleted via an `LV_EVENT_DELETE` callback on the map's root container (same pattern as Task 3; the rebuild deletes the old container which kills the old timer, and the fresh create arms a new one).

- [ ] **Step 4:** Build gate + commit `feat(ui_gfx): live map with honest peer counts and non-sticky focus`.

---

### Task 5: Traffic: Settings toggle + live monitor

**Files:**
- Modify: `components/ui_graphics/screens/scr_settings.c` (toggle row near the Reboot button)
- Modify: `components/ui_graphics/screens/scr_traffic.c` (live refresh, empty-state text)

- [ ] **Step 1:** Settings toggle. Before the Reboot button in `scr_settings_create`, add a `create_setting_row(cont, "Traffic Debug")` with a switch reflecting `traffic_debug_is_enabled(mesh_get_traffic_debug())`; the change callback calls `traffic_debug_enable(mesh_get_traffic_debug(), on)` and toasts "Traffic debug on/off". Declare `extern traffic_debug_t* mesh_get_traffic_debug(void);` and include `traffic_debug.h`.

- [ ] **Step 2:** Fix the dead-end empty-state text in `scr_traffic.c` ("Enable it in Config -> Traffic Debug" becomes "Enable Traffic Debug in Settings").

- [ ] **Step 3:** Live refresh: static `s_traffic_layout` + `s_last_count`; a 2 s timer that compares `traffic_debug_get_count(td)` (plus dropped) and rebuilds the screen (clean content + `scr_traffic_create`) only when changed, capturing and restoring the event list's scroll offset across the rebuild (static `s_list_scroll_y`, restore after build). Timer deleted via `LV_EVENT_DELETE` on the header (rebuild re-arms, same as Task 4).

- [ ] **Step 4:** Build gate + commit `feat(ui_gfx): live traffic monitor with an actual Settings toggle`.

---

### Task 6: Gate, PR, adversarial review, merge

- [ ] Full gate (host suite + three boards), clang-format changed files, commit plan, push, open PR `feat(ui_gfx): T-Deck liveness and input batch` via Gitea API.
- [ ] Run /code-review (workflow, high) on the diff; fix confirmed findings; push.
- [ ] Poll CI; merge on green (standing authorization; review pass satisfied).
