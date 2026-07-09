# T-Deck Messenger Core Loop (Batch 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the T-Deck's core messenger loop work: incoming messages wake the screen, reading is never yanked, DMs get unread badges and a way to start them, bubbles get timestamps, and failed actions say so on screen.

**Architecture:** Pure logic stays in host-tested modules (`chat_unread.c`, `chat_message_ui.c`); LVGL work stays in `screens/` and a new tiny `ui_toast.c`; power behavior stays in `sleep_manager.c` plus one new getter in the display driver. No mesh or msg_store changes.

**Tech Stack:** ESP-IDF C, LVGL v9, Unity host tests (`test/CMakeLists.txt`, run via `bash test/run_all_tests.sh`).

## Global Constraints

- No em dashes anywhere (code, comments, commits, PR text). House rule.
- No AI attribution in commits or PRs. House rule.
- Conventional commit messages (`fix(ui_gfx): ...`, `feat(ui_gfx): ...`).
- Branch: `feat/tdeck-messenger-core` off `origin/main`, in the current worktree.
- Firmware gate: `bash scripts/flash.sh local tdeck-plus build` must succeed (the LVGL UI only compiles for the T-Deck). Host gate: `bash test/run_all_tests.sh`.
- Existing host test targets `test_chat_unread` and `test_chat_message_ui` already compile the modules directly (`test/CMakeLists.txt:1181-1189`, `:1322-1330`); extending their `.c` files needs no CMake change.
- LVGL screens rebuild via `lv_obj_clean`; always `lv_refr_now(lv_display_get_default())` before cleaning (existing pattern, see `scr_layout.c:136`).

## Background (verified findings this fixes)

1. Incoming messages never wake the display: `sleep_manager_activity()` is called only from touch (`lv_port_touch.c:14`) and keyboard (`lv_port_keyboard.c:26`). The trackball cannot wake the device either (`lv_port_trackball.c`). Wake restores display backlight to a hardcoded 255 (`sleep_manager.c:102-104`).
2. Any incoming message re-renders the open conversation and force-scrolls to the bottom (`scr_chat_messages.c:425` via `scr_chat_messages_on_recv`), yanking a reader out of history. Expanding a route on an old bubble does the same.
3. DMs never count as unread: `chat_unread.c:16-20` drops `MSG_DIR_INCOMING` with `channel_index < 0`, and DM rows in the chat list get no badge.
4. There is no way to START a DM: the "+ Msg" picker (`scr_chat_compose.c`) lists only channels.
5. No timestamps on bubbles (`stored_msg_t.timestamp_s` unused by the LVGL UI).
6. Failed sends and failed/successful location shares produce only ESP_LOG output (`scr_chat_messages.c:113-116`, `scr_node_detail.c:53,60`).

---

### Task 0: Branch setup

**Files:** none (git only)

- [ ] **Step 1: Create the branch**

```bash
cd /home/justin/src/bramble/.claude/worktrees/electron
git fetch origin
git checkout -b feat/tdeck-messenger-core origin/main
```

- [ ] **Step 2: Confirm clean state**

Run: `git status --short`
Expected: only this plan file untracked (commit it in Task 7).

---

### Task 1: Toast helper (`ui_toast`)

**Files:**
- Create: `components/ui_graphics/include/ui_toast.h`
- Create: `components/ui_graphics/ui_toast.c`
- Modify: `components/ui_graphics/CMakeLists.txt` (add `"ui_toast.c"` to `ui_gfx_srcs`)

**Interfaces:**
- Produces: `void ui_toast_show(const char* text);` (transient 2.5 s overlay, bottom-center, safe to call repeatedly; each call replaces the prior toast). Tasks 6 and 7 consume it.
- No host test (pure LVGL); gate is the tdeck build in Step 3.

- [ ] **Step 1: Write the header and implementation**

`components/ui_graphics/include/ui_toast.h`:

```c
#ifndef BRAMBLE_UI_TOAST_H
#define BRAMBLE_UI_TOAST_H

/* Transient on-screen feedback. Shows text bottom-center on the top layer
 * for ~2.5 s. Each call replaces any toast currently showing. Safe to call
 * from LVGL timer/event context only (same task as lv_timer_handler). */
void ui_toast_show(const char* text);

#endif
```

`components/ui_graphics/ui_toast.c`:

```c
#include "ui_toast.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"

#define TOAST_DURATION_MS 2500

static lv_obj_t* s_toast = NULL;
static lv_timer_t* s_timer = NULL;

static void toast_dismiss(void) {
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

static void toast_timer_cb(lv_timer_t* timer) {
    (void)timer;
    s_timer = NULL; /* one-shot: LVGL deletes it after repeat_count hits 0 */
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

void ui_toast_show(const char* text) {
    if (!text || !text[0])
        return;

    toast_dismiss();

    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_radius(s_toast, BR_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_toast, 10, 0);
    lv_obj_set_style_pad_ver(s_toast, 6, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -48);

    lv_obj_t* lbl = lv_label_create(s_toast);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(lbl, 280, 0);

    s_timer = lv_timer_create(toast_timer_cb, TOAST_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}
```

- [ ] **Step 2: Register in the component build**

In `components/ui_graphics/CMakeLists.txt`, add to `ui_gfx_srcs` after `"chat_message_ui.c"`:

```cmake
        "ui_toast.c"
```

- [ ] **Step 3: Build gate**

Run: `bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2`
Expected: `==> Done!`

- [ ] **Step 4: Commit**

```bash
git add components/ui_graphics
git commit -m "feat(ui_gfx): transient toast helper for on-screen feedback"
```

---

### Task 2: Messages wake the screen; trackball wakes; honest backlight restore

**Files:**
- Modify: `components/display/include/display.h` (declare getter)
- Modify: `components/display/st7789.c` (cache + getter)
- Modify: `components/display/ssd1306.c` (stub getter)
- Modify: `components/ui_graphics/sleep_manager.c:100-104` (use getter)
- Modify: `components/ui_graphics/lv_port_trackball.c` (activity on input)
- Modify: `components/ui_graphics/ui_graphics.c:51` (activity on message)

**Interfaces:**
- Produces: `uint8_t display_get_backlight(void);` (last level passed to `display_set_backlight`, 255 before any call).

- [ ] **Step 1: Display backlight getter**

`components/display/include/display.h`, after the `display_set_backlight` declaration (line 62):

```c
/** Last level passed to display_set_backlight (255 before any call). */
uint8_t display_get_backlight(void);
```

`components/display/st7789.c`: add near the top statics:

```c
static uint8_t s_backlight_level = 255;
```

In `display_set_backlight` (line 457), record the level as the first statement of the function body:

```c
    s_backlight_level = level;
```

At the end of the file:

```c
uint8_t display_get_backlight(void) { return s_backlight_level; }
```

`components/display/ssd1306.c`, next to its no-op `display_set_backlight` (line 263):

```c
uint8_t display_get_backlight(void) { return 255; }
```

- [ ] **Step 2: Sleep manager saves the real level**

In `components/ui_graphics/sleep_manager.c:100-104`, replace:

```c
        /* Save current backlight levels before turning off */
        s_sleep.saved_kbd_backlight = keyboard_get_backlight_percent();
        /* Note: display backlight is controlled by LEDC PWM but we don't have
         * a get_backlight() API yet. For now, we assume it's at max (255). */
        s_sleep.saved_disp_backlight = 255;
```

with:

```c
        /* Save current backlight levels before turning off */
        s_sleep.saved_kbd_backlight = keyboard_get_backlight_percent();
        s_sleep.saved_disp_backlight = display_get_backlight();
```

- [ ] **Step 3: Trackball wakes the device**

In `components/ui_graphics/lv_port_trackball.c`, add `#include "sleep_manager.h"` after the existing includes, and in `trackball_read_cb`, right after `ui_button_t btn = trackball_poll();`:

```c
    if (btn != BTN_NONE) {
        sleep_manager_activity();
    }
```

- [ ] **Step 4: Incoming messages wake the display**

In `components/ui_graphics/ui_graphics.c` (`sleep_manager.h` is already included at line 6), in `status_refresh_timer_cb`, inside the `if ((events & UI_EVT_MSG_RECEIVED) && msg_received > 0)` block (line 51), add as the first statement:

```c
        /* A message is user-relevant activity: wake the display so the
         * unread badge is actually visible. */
        sleep_manager_activity();
```

- [ ] **Step 5: Build gate**

Run: `bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2` and `bash scripts/flash.sh local heltec-v3 build 2>&1 | tail -2` (display component is shared).
Expected: both `==> Done!`

- [ ] **Step 6: Commit**

```bash
git add components/display components/ui_graphics
git commit -m "fix(ui_gfx): wake the display on incoming messages and trackball input

The sleep manager only woke for touch and keyboard, so a sleeping
T-Deck received messages invisibly and the trackball could not wake
it. Wake restore also stomped the display backlight to 255; the
driver now remembers the last level set."
```

---

### Task 3: DM unread counts + badges

**Files:**
- Modify: `components/ui_graphics/include/chat_unread.h`
- Modify: `components/ui_graphics/chat_unread.c`
- Modify: `components/ui_graphics/screens/scr_chat_list.c:189-218` (badge on DM rows)
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (clear on DM open)
- Test: `test/test_chat_unread.c`

**Interfaces:**
- Produces:

```c
int chat_unread_count_for_dm(uint32_t peer_addr);
void chat_unread_clear_for_dm(uint32_t peer_addr);
```

`chat_unread_mark_for_message` now treats `MSG_DIR_INCOMING` with `channel_index < 0` as a DM keyed by `peer_addr` (up to 8 tracked peers; further peers are dropped). `chat_unread_reset` clears DM state too.

- [ ] **Step 1: Write the failing tests**

Add to `test/test_chat_unread.c` (register each in `main` with `RUN_TEST`):

```c
void test_incoming_dm_without_channel_counts_by_peer(void) {
    stored_msg_t msg = {
        .direction = MSG_DIR_INCOMING,
        .channel_index = -1,
        .peer_addr = 0xA1B2C3D4,
    };

    chat_unread_mark_for_message(&msg);
    chat_unread_mark_for_message(&msg);

    TEST_ASSERT_EQUAL(2, chat_unread_count_for_dm(0xA1B2C3D4));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0x11111111));
    /* DM traffic must not leak into channel counters */
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(0));
}

void test_clear_dm_only_clears_that_peer(void) {
    stored_msg_t a = {.direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0xAAAA0001};
    stored_msg_t b = {.direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0xBBBB0002};
    chat_unread_mark_for_message(&a);
    chat_unread_mark_for_message(&b);

    chat_unread_clear_for_dm(0xAAAA0001);

    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0xAAAA0001));
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_dm(0xBBBB0002));
}

void test_dm_peer_table_caps_at_eight(void) {
    for (uint32_t i = 0; i < 10; i++) {
        stored_msg_t m = {
            .direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0x1000 + i};
        chat_unread_mark_for_message(&m);
    }
    /* First eight tracked, rest dropped */
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_dm(0x1000));
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_dm(0x1007));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0x1008));
}

void test_reset_clears_dm_counts(void) {
    stored_msg_t m = {.direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0xCAFE0001};
    chat_unread_mark_for_message(&m);
    chat_unread_reset();
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0xCAFE0001));
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cd test && mkdir -p build && cd build && cmake .. >/dev/null && make test_chat_unread 2>&1 | tail -3
```
Expected: compile FAIL (`chat_unread_count_for_dm` undeclared).

- [ ] **Step 3: Implement**

`components/ui_graphics/include/chat_unread.h`: add declarations (with `#include <stdint.h>` if missing):

```c
/* DM unread, keyed by peer address (up to 8 tracked peers). */
int chat_unread_count_for_dm(uint32_t peer_addr);
void chat_unread_clear_for_dm(uint32_t peer_addr);
```

`components/ui_graphics/chat_unread.c`: add after the channel array:

```c
#define CHAT_UNREAD_MAX_DMS 8

typedef struct {
    uint32_t peer_addr; /* 0 = free slot */
    int count;
} dm_unread_t;

static dm_unread_t s_dm_unread[CHAT_UNREAD_MAX_DMS];

static dm_unread_t* dm_slot(uint32_t peer_addr, bool create) {
    dm_unread_t* free_slot = NULL;
    for (int i = 0; i < CHAT_UNREAD_MAX_DMS; i++) {
        if (s_dm_unread[i].peer_addr == peer_addr)
            return &s_dm_unread[i];
        if (!s_dm_unread[i].peer_addr && !free_slot)
            free_slot = &s_dm_unread[i];
    }
    if (create && free_slot) {
        free_slot->peer_addr = peer_addr;
        free_slot->count = 0;
        return free_slot;
    }
    return NULL;
}
```

Change `chat_unread_mark_for_message` to route DMs before the channel path:

```c
void chat_unread_mark_for_message(const stored_msg_t* msg) {
    if (!msg)
        return;

    if (msg->direction == MSG_DIR_INCOMING && msg->channel_index < 0 && msg->peer_addr != 0) {
        dm_unread_t* slot = dm_slot(msg->peer_addr, true);
        if (slot)
            slot->count++;
        return;
    }

    int channel = normalize_channel_for_unread(msg);
    if (channel < 0 || channel >= CHAT_UNREAD_MAX_CHANNELS)
        return;
    s_unread_counts[channel]++;
}
```

Add the accessors and extend reset:

```c
int chat_unread_count_for_dm(uint32_t peer_addr) {
    dm_unread_t* slot = dm_slot(peer_addr, false);
    return slot ? slot->count : 0;
}

void chat_unread_clear_for_dm(uint32_t peer_addr) {
    dm_unread_t* slot = dm_slot(peer_addr, false);
    if (slot) {
        slot->peer_addr = 0;
        slot->count = 0;
    }
}

void chat_unread_reset(void) {
    memset(s_unread_counts, 0, sizeof(s_unread_counts));
    memset(s_dm_unread, 0, sizeof(s_dm_unread));
}
```

(Delete the old one-line `chat_unread_reset`. `normalize_channel_for_unread` keeps handling `MSG_DIR_INCOMING` with `channel_index >= 0` as channel traffic, so the existing channel tests stay green. Add `#include <stdbool.h>` if the compiler complains.)

- [ ] **Step 4: Run tests**

```bash
cd test/build && make test_chat_unread >/dev/null && ./test_chat_unread
```
Expected: PASS (old + 4 new).

- [ ] **Step 5: Badge DM rows and clear on open**

In `components/ui_graphics/screens/scr_chat_list.c`, add `#include <stdbool.h>` if needed (chat_unread.h is already included). In the DM card loop (after the label alignment at line 212, before `lv_obj_add_event_cb`), add the same badge pattern the channel cards use:

```c
        int dm_unread = chat_unread_count_for_dm(dm_peers[i]);
        if (dm_unread > 0) {
            lv_obj_t* badge = lv_obj_create(card);
            lv_obj_set_size(badge, 26, 18);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(badge, BR_COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(badge, 9, 0);
            lv_obj_set_style_border_width(badge, 0, 0);
            lv_obj_set_style_pad_all(badge, 0, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* badge_lbl = lv_label_create(badge);
            lv_label_set_text_fmt(badge_lbl, "%d", dm_unread);
            lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(badge_lbl, lv_color_white(), 0);
            lv_obj_center(badge_lbl);
        }
```

In `components/ui_graphics/screens/scr_chat_messages.c`, `scr_chat_messages_open_dm` (line 551), clear the peer's unread before opening:

```c
void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr) {
    chat_unread_clear_for_dm(peer_addr);
    open_with_target(layout, chat_target_dm(peer_addr), -1);
}
```

(`chat_unread.h` is already included there via `#include "chat_unread.h"`, line 6.)

- [ ] **Step 6: Build gate + host suite**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2
bash test/run_all_tests.sh 2>&1 | tail -2
```
Expected: build Done, all suites pass.

- [ ] **Step 7: Commit**

```bash
git add components/ui_graphics test/test_chat_unread.c
git commit -m "feat(ui_gfx): DM unread tracking and badges

DMs (incoming, no channel) were invisible to the unread system. Track
them per peer, badge the DM rows in the chat list, and clear on open."
```

---

### Task 4: Stop yanking the reader (scroll preservation)

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_messages.c`

**Interfaces:**
- Internal only: `render_messages_for_target(void)` becomes `render_messages_for_target(bool scroll_to_bottom)`. Callers: open (`true`), send success (`true`), target cycle (`true`), route-toggle click (`false`), `scr_chat_messages_on_recv` (`false`).
- Behavior with `false`: if the reader was at (or within 8 px of) the bottom before re-render, follow to the new bottom; otherwise restore the previous scroll offset.

- [ ] **Step 1: Change the renderer**

In `components/ui_graphics/screens/scr_chat_messages.c`, replace the declaration (line 21) and the function (lines 391-426):

```c
static void render_messages_for_target(bool scroll_to_bottom);
```

```c
static void render_messages_for_target(bool scroll_to_bottom) {
    if (!s_msg_list)
        return;

    /* Preserve the reading position unless explicitly asked to jump: a
     * reader scrolled into history must not be yanked by an arrival. */
    int32_t prev_y = lv_obj_get_scroll_y(s_msg_list);
    bool was_at_bottom = (lv_obj_get_scroll_bottom(s_msg_list) <= 8);

    lv_refr_now(lv_display_get_default());
    lv_obj_clean(s_msg_list);

    int count = msg_store_count();
    for (int i = 0; i < count; i++) {
        const stored_msg_t* msg = msg_store_get(i);
        if (!msg || !message_matches_target(msg))
            continue;

        bool is_mine =
            (msg->direction == MSG_DIR_OUTGOING || msg->direction == MSG_DIR_BROADCAST_OUT);

        char sender[20];
        if (!is_mine) {
            /* Try to get peer name first, fallback to hex address */
            const char* peer_name = mesh_get_peer_name(msg->peer_addr);
            if (peer_name) {
                snprintf(sender, sizeof(sender), "%s", peer_name);
            } else {
                snprintf(sender, sizeof(sender), "%08lX", (unsigned long)msg->peer_addr);
            }
        }

        if (msg_is_action(msg)) {
            add_action_line(s_msg_list, sender, msg, is_mine);
        } else {
            add_message_bubble(s_msg_list, is_mine ? NULL : sender, msg, is_mine);
        }
    }

    if (scroll_to_bottom || was_at_bottom) {
        lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(s_msg_list, prev_y, LV_ANIM_OFF);
    }
}
```

- [ ] **Step 2: Update the callers**

- `channel_cycle_click_cb` (line 65): `render_messages_for_target(true);`
- `send_current_message` success path (line 112): `render_messages_for_target(true);`
- `msg_bubble_click_cb` (line 140): `render_messages_for_target(false);`
- `open_with_target` (line 503): `render_messages_for_target(true);`
- `scr_chat_messages_on_recv` (line 555): `render_messages_for_target(false);`

- [ ] **Step 3: Build gate**

Run: `bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2`
Expected: `==> Done!`

- [ ] **Step 4: Commit**

```bash
git add components/ui_graphics
git commit -m "fix(ui_gfx): stop yanking the conversation to the bottom on arrivals

Re-renders (incoming message, route toggle) now preserve the reading
position; only opening, sending, and switching targets jump to the
newest message. A reader already at the bottom still follows new
messages."
```

---

### Task 5: Message ages on bubbles

**Files:**
- Modify: `components/ui_graphics/include/chat_message_ui.h`
- Modify: `components/ui_graphics/chat_message_ui.c`
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (age label per bubble)
- Test: `test/test_chat_message_ui.c`

**Interfaces:**
- Produces: `int chat_format_age(uint32_t age_s, char* buf, size_t buf_len);` writing `"now"` (< 60 s), `"5m"`, `"3h"`, or `"2d"`.

- [ ] **Step 1: Write the failing tests**

Add to `test/test_chat_message_ui.c` (register in `main`):

```c
void test_format_age_under_a_minute_is_now(void) {
    char buf[8];
    chat_format_age(45, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("now", buf);
}

void test_format_age_minutes_hours_days(void) {
    char buf[8];
    chat_format_age(300, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("5m", buf);
    chat_format_age(7200, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2h", buf);
    chat_format_age(172800, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2d", buf);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cd test/build && make test_chat_message_ui 2>&1 | tail -3
```
Expected: compile FAIL (`chat_format_age` undeclared).

- [ ] **Step 3: Implement**

`components/ui_graphics/include/chat_message_ui.h`: add (needs `<stddef.h>` and `<stdint.h>`, include if missing):

```c
/* Compact age for message bubbles: "now", "5m", "3h", "2d". */
int chat_format_age(uint32_t age_s, char* buf, size_t buf_len);
```

`components/ui_graphics/chat_message_ui.c`: add (include `<stdio.h>` if missing):

```c
int chat_format_age(uint32_t age_s, char* buf, size_t buf_len) {
    if (!buf || buf_len == 0)
        return 0;
    if (age_s < 60)
        return snprintf(buf, buf_len, "now");
    if (age_s < 3600)
        return snprintf(buf, buf_len, "%um", (unsigned)(age_s / 60));
    if (age_s < 86400)
        return snprintf(buf, buf_len, "%uh", (unsigned)(age_s / 3600));
    return snprintf(buf, buf_len, "%ud", (unsigned)(age_s / 86400));
}
```

- [ ] **Step 4: Run tests**

```bash
cd test/build && make test_chat_message_ui >/dev/null && ./test_chat_message_ui
```
Expected: PASS.

- [ ] **Step 5: Render ages in bubbles**

In `components/ui_graphics/screens/scr_chat_messages.c`:

Add `#include "esp_timer.h"` to the includes if not present.

In `add_message_bubble`, add an `age_s` parameter: change the signature (and its forward uses) from

```c
static void add_message_bubble(lv_obj_t* parent, const char* sender, const stored_msg_t* msg,
                               bool is_mine)
```

to

```c
static void add_message_bubble(lv_obj_t* parent, const char* sender, const stored_msg_t* msg,
                               bool is_mine, uint32_t age_s)
```

At the end of `add_message_bubble` (after the route-toggle block), append the age label:

```c
    char age_buf[8];
    chat_format_age(age_s, age_buf, sizeof(age_buf));
    lv_obj_t* age_lbl = lv_label_create(bubble);
    lv_label_set_text(age_lbl, age_buf);
    lv_obj_set_style_text_font(age_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(age_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_align(age_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(age_lbl, LV_PCT(100));
```

(`chat_message_ui.h` is already included at line 7.)

In `render_messages_for_target`, before the loop compute now:

```c
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
```

and at the bubble call site:

```c
            uint32_t age_s = (now_s >= msg->timestamp_s) ? (now_s - msg->timestamp_s) : 0;
            add_message_bubble(s_msg_list, is_mine ? NULL : sender, msg, is_mine, age_s);
```

- [ ] **Step 6: Build gate**

Run: `bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2`
Expected: `==> Done!`

- [ ] **Step 7: Commit**

```bash
git add components/ui_graphics test/test_chat_message_ui.c
git commit -m "feat(ui_gfx): message age on chat bubbles"
```

---

### Task 6: Start a DM from the compose picker

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_compose.c`

**Interfaces:**
- Consumes: `ui_shared_mesh_state()` (`screens/ui_shared_state.h`, returns `const ui_mesh_state_t*` with `.neighbors.count` and `.neighbors.entries[i].addr/.name`), `scr_chat_messages_open_dm(bramble_layout_t*, uint32_t)` (Task 3 clears unread inside it), `mesh_get_peer_name(uint32_t)`.

- [ ] **Step 1: Add a Direct section to the picker**

In `components/ui_graphics/screens/scr_chat_compose.c`:

Add includes and externs near the top:

```c
#include "ui_shared_state.h"
```

```c
extern void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr);
```

Add a DM click callback next to `target_click_cb`:

```c
static void dm_target_click_cb(lv_event_t* e) {
    uint32_t peer_addr = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    extern bramble_layout_t* s_layout;
    scr_chat_messages_open_dm(s_layout, peer_addr);
}
```

In `scr_chat_compose_open`, after the channel-card loop (line 115) and before the hint label, add:

```c
    /* Direct messages: one card per known neighbor */
    const ui_mesh_state_t* mesh = ui_shared_mesh_state();
    for (int i = 0; i < mesh->neighbors.count; i++) {
        uint32_t addr = mesh->neighbors.entries[i].addr;
        if (addr == 0)
            continue;

        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 40);
        lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, BR_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_FOCUSED);
        if (g)
            lv_group_add_obj(g, card);

        lv_obj_t* lbl = lv_label_create(card);
        const char* name = mesh->neighbors.entries[i].name;
        char dm_buf[48];
        if (name && name[0]) {
            snprintf(dm_buf, sizeof(dm_buf), "@ %s", name);
        } else {
            snprintf(dm_buf, sizeof(dm_buf), "@ %08lX", (unsigned long)addr);
        }
        lv_label_set_text(lbl, dm_buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_event_cb(card, dm_target_click_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)addr);
    }
```

Also change the hint text (line 118) to:

```c
    lv_label_set_text(hint, "Channels broadcast to members; @ names are direct messages.");
```

- [ ] **Step 2: Fix the double-build on Back while here**

`back_click_cb` (lines 13-20) builds the chat list twice (`scr_chat_list_refresh` already rebuilds via `layout_set_tab`). Replace the body with:

```c
static void back_click_cb(lv_event_t* e) {
    bramble_layout_t* layout = (bramble_layout_t*)lv_event_get_user_data(e);
    /* Show tab bar */
    lv_obj_clear_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
    /* Return to chat list (layout_set_tab cleans and rebuilds) */
    scr_chat_list_refresh(layout);
}
```

- [ ] **Step 3: Build gate**

Run: `bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2`
Expected: `==> Done!`

- [ ] **Step 4: Commit**

```bash
git add components/ui_graphics
git commit -m "feat(ui_gfx): start DMs from the compose picker

The + Msg flow only listed channels, so there was no way to start a
new DM from the device. List known neighbors as direct targets. Also
fix the back button building the chat list twice."
```

---

### Task 7: Visible failure feedback + verification + PR

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (send failure toast)
- Modify: `components/ui_graphics/screens/scr_node_detail.c` (share-location toasts)
- Commit: `docs/plans/2026-07-09-tdeck-messenger-core.md`

**Interfaces:**
- Consumes: `ui_toast_show` (Task 1).

- [ ] **Step 1: Send failure toast**

In `components/ui_graphics/screens/scr_chat_messages.c`, add `#include "ui_toast.h"`, and in `send_current_message`'s failure branch (line 113-116), add after the `ESP_LOGW`:

```c
        ui_toast_show("Send failed");
```

- [ ] **Step 2: Share-location feedback**

In `components/ui_graphics/screens/scr_node_detail.c`, add `#include "ui_toast.h"`. In the share-location callback, after the success `ESP_LOGI` (line 60) add:

```c
    ui_toast_show("Location shared");
```

and after the no-fix `ESP_LOGW` early-return path (line 53) add, before the return:

```c
        ui_toast_show("No position to share yet");
```

(Read the function first; keep the toast text on the matching branch.)

- [ ] **Step 3: Full gate**

```bash
bash test/run_all_tests.sh 2>&1 | tail -2
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -2
bash scripts/flash.sh local heltec-v3 build 2>&1 | tail -2
bash scripts/flash.sh local heltec-v4 build 2>&1 | tail -2
bash scripts/check-rpc-contract.sh | tail -1
```
Expected: everything green.

- [ ] **Step 4: clang-format the changed files**

```bash
git diff origin/main...HEAD --name-only | grep -E '\.(c|h)$' | xargs clang-format -i
git diff --stat
bash test/run_all_tests.sh 2>&1 | tail -2
```
Commit any formatting deltas as `style: clang-format the tdeck messenger batch`.

- [ ] **Step 5: Push and open PR (Gitea API, not gh)**

```bash
git add docs/plans/2026-07-09-tdeck-messenger-core.md
git commit -m "docs(plans): tdeck messenger core batch plan"
git push -u origin feat/tdeck-messenger-core
PAT=$(cat ~/src/bramble-meta/secrets/gitea-pat)
curl -sS -X POST -H "Authorization: token $PAT" -H "Content-Type: application/json" \
  "https://git.idiotica.org/api/v1/repos/dumbot/bramble/pulls" \
  -d '{"title": "feat(ui_gfx): T-Deck messenger core loop batch", "head": "feat/tdeck-messenger-core", "base": "main", "body": "..."}'
```
PR body: the six findings fixed (wake-on-message, scroll preservation, DM unread, DM entry point, ages, toasts) + test plan (host suites, three board builds, on-device checklist: sleep then send a message and watch it wake; scroll up in a conversation while messages arrive; DM badge + open from + Msg; failed send shows toast).

- [ ] **Step 6: Wait for CI green** (poll `GET /repos/dumbot/bramble/commits/{sha}/statuses`), fix failures.

- [ ] **Step 7: Report PR URL and STOP.** Merging requires explicit user instruction (house rule). Remind: V3 flashes with `--encrypt`.
