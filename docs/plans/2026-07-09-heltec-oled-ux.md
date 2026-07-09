# Heltec OLED UX Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Heltec OLED UI a trustworthy pager: reliable new-message notification, sender identity and channel names in chat, unread badge, reader-friendly timeouts, scrollback, and discoverable button hints.

**Architecture:** All state-machine logic lives in `components/ui` (host-testable, no display deps); all pixel work stays in `main/main.c` render functions. A new monotonic incoming-message counter in `components/msg_store` replaces the broken count-based arrival detector. A new pure formatter in `ui_format.c` renders one message line (identity, channel tag, action, badge) so it is unit-tested without a display.

**Tech Stack:** ESP-IDF C (firmware), Unity host tests under `test/` (compiled with `test/CMakeLists.txt`, run via `bash test/run_all_tests.sh`).

## Global Constraints

- No em dashes anywhere (code, comments, commits, PR text). House rule.
- No AI attribution in commits or PRs (no Co-Authored-By, no session links). House rule.
- Conventional commit messages, e.g. `fix(ui): ...`, `feat(ui): ...`.
- The OLED is 128x64: `CHARS_PER_LINE` = 21, 4 content lines between `CONTENT_Y` (14) and `FOOTER_Y` (54), `LINE_H` = 10 (constants in `main/main.c:56-64`).
- Heltec has ONE button: `BTN_SHORT_PRESS`, `BTN_LONG_PRESS`, `BTN_DOUBLE_PRESS`. Trackball buttons (`BTN_UP/DOWN/LEFT/RIGHT/SELECT`) are the non-graphical T-Deck fallback; do not regress their handling but do not extend it.
- Branch: `feat/heltec-oled-ux` off `origin/main`. Work happens in the current worktree.
- Host tests are the CI gate: `bash test/run_all_tests.sh` must pass before every push.

## Background: the two bugs this fixes

1. `main/main.c:1321` detects new messages by `msg_store_count()` increasing, but the store is a 20-slot ring (`MSG_STORE_MAX`, `components/msg_store/msg_store.c:52`): the count saturates at 20 and the detector never fires again. It also fires on OUTGOING messages (sending from the webapp yanks the OLED to Messages).
2. When a message arrives within 10 s of a button press, `ui_on_message_received` sets `pending_message_notification` (`components/ui/ui_manager.c:181`), which no render code reads, and which the next button press clears unseen (`ui_manager.c:20`).

---

### Task 0: Branch setup

**Files:** none (git only)

- [ ] **Step 1: Create the branch**

```bash
cd /home/justin/src/bramble/.claude/worktrees/electron
git fetch origin
git checkout -b feat/heltec-oled-ux origin/main
```

- [ ] **Step 2: Confirm clean state**

Run: `git status --short`
Expected: empty output (the plan file itself may show as untracked; commit it in Task 7 or leave it).

---

### Task 1: Monotonic incoming-message counter in msg_store

**Files:**
- Modify: `components/msg_store/include/msg_store.h`
- Modify: `components/msg_store/msg_store.c`
- Test: `test/test_msg_store.c`

**Interfaces:**
- Produces: `uint32_t msg_store_total_incoming(void);` a monotonic counter of stored messages whose direction is `MSG_DIR_INCOMING` or `MSG_DIR_BROADCAST_IN`. Never decreases; resets to 0 only in `msg_store_init()`. Task 6 consumes it.

- [ ] **Step 1: Write the failing test**

Add to `test/test_msg_store.c` (alongside the existing tests; register with `RUN_TEST` in its `main`):

```c
void test_total_incoming_is_monotonic_and_ignores_outgoing(void) {
    msg_store_init();
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_total_incoming());

    msg_store_add(0x11111111, MSG_DIR_INCOMING, "hi", 2, -70, 5);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_total_incoming());

    msg_store_add(0x22222222, MSG_DIR_OUTGOING, "yo", 2, 0, 0);
    msg_store_add(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, "b", 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_total_incoming());

    msg_store_add(0x33333333, MSG_DIR_BROADCAST_IN, "bc", 2, -80, 3);
    TEST_ASSERT_EQUAL_UINT32(2, msg_store_total_incoming());

    /* Keep counting past the 20-slot ring capacity (the whole point). */
    for (int i = 0; i < 25; i++) {
        msg_store_add(0x44444444, MSG_DIR_INCOMING, "x", 1, -80, 3);
    }
    TEST_ASSERT_EQUAL_UINT32(27, msg_store_total_incoming());
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd test && mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug >/dev/null && make test_msg_store 2>&1 | tail -5
```
Expected: compile FAIL, `msg_store_total_incoming` undeclared.

- [ ] **Step 3: Implement**

In `components/msg_store/include/msg_store.h`, next to `int msg_store_count(void);` add:

```c
/* Monotonic count of INCOMING messages ever stored (MSG_DIR_INCOMING or
 * MSG_DIR_BROADCAST_IN). Unlike msg_store_count(), this never saturates at
 * the ring capacity, so callers can detect arrivals by delta. Reset only by
 * msg_store_init(). */
uint32_t msg_store_total_incoming(void);
```

In `components/msg_store/msg_store.c`: add a static counter next to `s_count`, reset it in `msg_store_init()` (the function that sets `s_count = 0`), bump it in `msg_store_add_ex2` right after the `s_count` update, and add the accessor:

```c
static uint32_t s_total_incoming;
```

In `msg_store_init()` add: `s_total_incoming = 0;`

In `msg_store_add_ex2`, after the `if (s_count < MSG_STORE_MAX) { s_count++; }` block:

```c
    if (dir == MSG_DIR_INCOMING || dir == MSG_DIR_BROADCAST_IN) {
        s_total_incoming++;
    }
```

At the end of the file (near `msg_store_count`):

```c
uint32_t msg_store_total_incoming(void) { return s_total_incoming; }
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd test/build && make test_msg_store >/dev/null && ./test_msg_store
```
Expected: all tests PASS including `test_total_incoming_is_monotonic_and_ignores_outgoing`.

- [ ] **Step 5: Commit**

```bash
git add components/msg_store test/test_msg_store.c
git commit -m "feat(msg_store): monotonic incoming-message counter

msg_store_count() saturates at the 20-slot ring capacity, which silently
killed the OLED new-message detector after the 20th message. Expose a
monotonic incoming-only counter so arrival detection survives ring
wraparound and stops firing on outgoing sends."
```

---

### Task 2: Unread counter + press-to-view (ui_manager)

**Files:**
- Modify: `components/ui/include/ui.h`
- Modify: `components/ui/ui_manager.c`
- Test: `test/test_ui.c`

**Interfaces:**
- Consumes: nothing new.
- Produces: `ui_state_t.unread_count` (int, capped at 99), replacing the removed `pending_message_notification` bool. Semantics: incremented by `ui_on_message_received` when the message is not immediately shown; cleared to 0 whenever the UI lands on `SCREEN_MESSAGES`. A `BTN_SHORT_PRESS` anywhere outside `SCREEN_MESSAGES` and `SCREEN_SETTINGS` jumps straight to `SCREEN_MESSAGES` when `unread_count > 0` (press-to-view). Task 6 renders the badge from this field.

- [ ] **Step 1: Update the two existing tests and add three new ones**

In `test/test_ui.c`, replace `test_incoming_message_idle_auto_switches_to_messages` (line ~343) and `test_incoming_message_while_active_sets_pending_flag_without_switch` (line ~355) with:

```c
void test_incoming_message_idle_auto_switches_to_messages(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;

    ui_on_message_received(&state, 12050);

    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(SCREEN_NODES, state.prev_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
    TEST_ASSERT_TRUE(state.message_auto_switch_time > 0);
}

void test_incoming_message_while_active_increments_unread_without_switch(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 5000;

    ui_on_message_received(&state, 12000); /* 7s idle -> still active */
    ui_on_message_received(&state, 12500);

    TEST_ASSERT_EQUAL(SCREEN_NODES, state.current_screen);
    TEST_ASSERT_EQUAL(2, state.unread_count);
    TEST_ASSERT_EQUAL(0, state.message_auto_switch_time);
    TEST_ASSERT_TRUE(state.screen_dirty); /* badge must render */
}

void test_short_press_with_unread_jumps_to_messages_and_clears(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 5000;
    ui_on_message_received(&state, 12000);
    TEST_ASSERT_EQUAL(1, state.unread_count);

    ui_handle_button(&state, BTN_SHORT_PRESS, 12500);

    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(SCREEN_NODES, state.prev_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
}

void test_cycling_into_messages_clears_unread(void) {
    state.current_screen = SCREEN_MAIN;
    state.last_activity = 5000;
    ui_on_message_received(&state, 12000);
    TEST_ASSERT_EQUAL(1, state.unread_count);

    /* Press-to-view fires from MAIN too; land on messages either way. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 12500);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
}

void test_message_while_viewing_messages_does_not_count_unread(void) {
    state.current_screen = SCREEN_MESSAGES;
    state.last_activity = 5000;

    ui_on_message_received(&state, 12000);

    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
    TEST_ASSERT_TRUE(state.screen_dirty);
}
```

Register the three new tests in `main()` with `RUN_TEST(...)` next to the existing message tests.

- [ ] **Step 2: Run to verify failure**

```bash
cd test/build && cmake .. >/dev/null && make test_ui 2>&1 | tail -5
```
Expected: compile FAIL (`unread_count` not a member; `pending_message_notification` removed later, so first failure is the missing field).

- [ ] **Step 3: Implement in components/ui**

In `components/ui/include/ui.h`, in `ui_state_t`, replace:

```c
    bool pending_message_notification; /* set when a message arrives during active navigation */
```

with:

```c
    int unread_count; /* messages arrived but not yet seen; rendered as a header badge */
```

In `components/ui/ui_manager.c`:

(a) Delete the clear-on-any-button block (lines 19-21):

```c
    if (btn != BTN_NONE) {
        state->pending_message_notification = false;
    }
```

(b) In the global `switch (btn)` inside `ui_handle_button`, at the top of the `case BTN_SHORT_PRESS: / case BTN_RIGHT: / case BTN_DOWN:` block, add press-to-view (before the next-screen computation):

```c
        if (btn == BTN_SHORT_PRESS && state->unread_count > 0 &&
            state->current_screen != SCREEN_MESSAGES) {
            state->prev_screen = state->current_screen;
            state->current_screen = SCREEN_MESSAGES;
            state->screen_enter_time = now_ms;
            state->screen_dirty = true;
            break;
        }
```

(Settings short presses return earlier in the function, so this cannot hijack row cycling.)

(c) At the very end of `ui_handle_button` (after the `switch`), clear unread whenever we are on the messages screen:

```c
    if (state->current_screen == SCREEN_MESSAGES) {
        state->unread_count = 0;
    }
```

(d) Replace `ui_on_message_received` with:

```c
void ui_on_message_received(ui_state_t* state, uint32_t now_ms) {
    if (state->current_screen == SCREEN_MESSAGES) {
        /* Reader is already looking at the list: nothing pending, but
         * extend the auto-restore window if one is running and repaint. */
        if (state->message_auto_switch_time != 0) {
            state->message_auto_switch_time = now_ms;
        }
        state->screen_dirty = true;
        return;
    }

    uint32_t idle_ms = now_ms - state->last_activity;
    if (idle_ms >= UI_MESSAGE_IDLE_THRESHOLD_MS) {
        state->prev_screen = state->current_screen;
        state->current_screen = SCREEN_MESSAGES;
        state->screen_enter_time = now_ms;
        state->unread_count = 0;
        state->message_auto_switch_time = now_ms;
        state->screen_dirty = true;
    } else {
        if (state->unread_count < 99) {
            state->unread_count++;
        }
        state->screen_dirty = true;
    }
}
```

- [ ] **Step 4: Run tests**

```bash
cd test/build && make test_ui >/dev/null && ./test_ui
```
Expected: PASS (all, including the pre-existing auto-restore tests, which do not reference the removed flag except the two rewritten ones).

- [ ] **Step 5: Commit**

```bash
git add components/ui test/test_ui.c
git commit -m "fix(ui): count unread messages instead of a flag nobody renders

The old pending_message_notification bool was never drawn and was
cleared by the next button press unseen. Track an unread count, expose
it for a header badge, and make a short press jump straight to Messages
while anything is unread."
```

---

### Task 3: Reader-friendly timeouts (ui_manager)

**Files:**
- Modify: `components/ui/include/ui.h`
- Modify: `components/ui/ui_manager.c`
- Test: `test/test_ui.c`

**Interfaces:**
- Produces: `UI_MESSAGES_INACTIVITY_TIMEOUT_MS` (300000) in `ui.h`. `ui_check_timeout` bounces to MAIN after 60 s everywhere EXCEPT the messages screen, which gets 5 minutes.

- [ ] **Step 1: Update the existing test and add one**

In `test/test_ui.c`, replace `test_inactivity_timeout` (line ~49) with:

```c
void test_inactivity_timeout(void) {
    /* Non-messages screens bounce to MAIN after 60s. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    ui_handle_button(&state, BTN_SHORT_PRESS, 1100); /* -> SCREEN_NODES */
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

    ui_check_timeout(&state, 50000);
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

    ui_check_timeout(&state, 61101);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}

void test_messages_screen_gets_long_inactivity_timeout(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    /* 60s inactivity must NOT bounce a reader off the messages screen. */
    ui_check_timeout(&state, 62000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    /* But 5 minutes does. */
    ui_check_timeout(&state, 302000);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}
```

Register the new test with `RUN_TEST`.

- [ ] **Step 2: Run to verify failure**

```bash
cd test/build && make test_ui >/dev/null && ./test_ui
```
Expected: `test_messages_screen_gets_long_inactivity_timeout` FAILS (bounces at 62000).

- [ ] **Step 3: Implement**

In `components/ui/include/ui.h`, next to `UI_INACTIVITY_TIMEOUT_MS`:

```c
/* Reading is idle time: give the messages screen a much longer leash. */
#define UI_MESSAGES_INACTIVITY_TIMEOUT_MS 300000
```

In `ui_check_timeout` in `ui_manager.c`, replace the inactivity block:

```c
    if (state->current_screen != SCREEN_MAIN &&
        (now_ms - state->last_activity) >= UI_INACTIVITY_TIMEOUT_MS) {
```

with:

```c
    uint32_t inactivity_limit = (state->current_screen == SCREEN_MESSAGES)
                                    ? UI_MESSAGES_INACTIVITY_TIMEOUT_MS
                                    : UI_INACTIVITY_TIMEOUT_MS;
    if (state->current_screen != SCREEN_MAIN &&
        (now_ms - state->last_activity) >= inactivity_limit) {
```

(The 30 s auto-restore for idle auto-switched glances stays as is; Task 4 gates it on scroll.)

- [ ] **Step 4: Run tests**

```bash
cd test/build && make test_ui >/dev/null && ./test_ui
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add components/ui test/test_ui.c
git commit -m "fix(ui): stop yanking the screen away from a reader

Reading produces no button presses, so the 60s idle bounce kicked
readers back to MAIN mid-read. The messages screen now gets 5 minutes."
```

---

### Task 4: Message scrollback (ui_manager)

**Files:**
- Modify: `components/ui/include/ui.h`
- Modify: `components/ui/ui_manager.c`
- Test: `test/test_ui.c`

**Interfaces:**
- Produces:
  - `ui_state_t.msg_scroll` (int, 0 = newest page, grows toward older messages, units = messages)
  - `ui_state_t.msg_total` (int, snapshot of `msg_store_count()`)
  - `void ui_set_message_total(ui_state_t* state, int total);`
  - `#define UI_MSG_PAGE_LINES 4` in `ui.h` (the OLED shows 4 message lines)
  - On `SCREEN_MESSAGES`: `BTN_LONG_PRESS` pages older (clamped), `BTN_DOUBLE_PRESS` with `msg_scroll > 0` returns to newest instead of jumping screens.
  - Auto-restore (30 s) is suppressed while `msg_scroll > 0`.
  - Task 6 renders the window `[count - UI_MSG_PAGE_LINES - msg_scroll, ...)`.

- [ ] **Step 1: Write failing tests**

Add to `test/test_ui.c`:

```c
void test_long_press_on_messages_pages_older_and_clamps(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    ui_set_message_total(&state, 10);

    ui_handle_button(&state, BTN_LONG_PRESS, 2000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    ui_handle_button(&state, BTN_LONG_PRESS, 3000);
    TEST_ASSERT_EQUAL(6, state.msg_scroll); /* clamp: 10 - 4 */

    ui_handle_button(&state, BTN_LONG_PRESS, 4000);
    TEST_ASSERT_EQUAL(6, state.msg_scroll); /* stays clamped */
}

void test_double_press_returns_to_newest_when_scrolled(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    ui_set_message_total(&state, 10);
    ui_handle_button(&state, BTN_LONG_PRESS, 2000);
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 3000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state)); /* no screen jump */
    TEST_ASSERT_EQUAL(0, state.msg_scroll);

    /* A second double-press (not scrolled) is the normal back-jump. */
    ui_handle_button(&state, BTN_DOUBLE_PRESS, 4000);
    TEST_ASSERT_NOT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
}

void test_message_while_scrolled_counts_unread_and_blocks_restore(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;
    ui_on_message_received(&state, 12050); /* idle -> auto-switch */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    ui_set_message_total(&state, 10);
    ui_handle_button(&state, BTN_LONG_PRESS, 13000); /* scroll older */
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    /* New arrival while reading history: counted, no forced jump. */
    ui_on_message_received(&state, 13500);
    TEST_ASSERT_EQUAL(1, state.unread_count);
    TEST_ASSERT_EQUAL(4, state.msg_scroll);
}

void test_entering_messages_resets_scroll(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    ui_set_message_total(&state, 10);
    ui_handle_button(&state, BTN_LONG_PRESS, 2000);
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 3000); /* newest */
    ui_handle_button(&state, BTN_DOUBLE_PRESS, 3500); /* leave */
    ui_handle_button(&state, BTN_SHORT_PRESS, 4000);  /* re-enter messages */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(0, state.msg_scroll);
}
```

Register all four with `RUN_TEST`.

- [ ] **Step 2: Run to verify failure**

```bash
cd test/build && make test_ui 2>&1 | tail -3
```
Expected: compile FAIL (`ui_set_message_total` undeclared).

- [ ] **Step 3: Implement**

In `components/ui/include/ui.h`:

Add to `ui_state_t`:

```c
    int msg_scroll; /* messages scrolled back from newest on SCREEN_MESSAGES */
    int msg_total;  /* snapshot of msg_store_count(), set by the main loop */
```

Add near the other defines:

```c
#define UI_MSG_PAGE_LINES 4 /* message lines visible on the 128x64 OLED */
```

Add declaration:

```c
void ui_set_message_total(ui_state_t* state, int total);
```

In `components/ui/ui_manager.c`:

(a) Add the setter:

```c
void ui_set_message_total(ui_state_t* state, int total) { state->msg_total = total; }
```

(b) In `ui_handle_button`, AFTER the settings blocks (which return) and BEFORE the global `switch (btn)`, add a messages block:

```c
    if (state->current_screen == SCREEN_MESSAGES) {
        if (btn == BTN_LONG_PRESS) {
            int max_scroll = (state->msg_total > UI_MSG_PAGE_LINES)
                                 ? state->msg_total - UI_MSG_PAGE_LINES
                                 : 0;
            int next = state->msg_scroll + UI_MSG_PAGE_LINES;
            state->msg_scroll = (next > max_scroll) ? max_scroll : next;
            state->screen_dirty = true;
            return;
        }
        if (btn == BTN_DOUBLE_PRESS && state->msg_scroll > 0) {
            state->msg_scroll = 0;
            state->screen_dirty = true;
            return;
        }
    }
```

(c) Reset scroll whenever the UI lands on the messages screen. Extend the tail added in Task 2:

```c
    if (state->current_screen == SCREEN_MESSAGES) {
        state->unread_count = 0;
    }
```

becomes:

```c
    if (state->current_screen == SCREEN_MESSAGES) {
        state->unread_count = 0;
        if (state->prev_screen != SCREEN_MESSAGES) {
            state->msg_scroll = 0;
        }
    }
```

Also reset in the auto-switch path of `ui_on_message_received` (add `state->msg_scroll = 0;` next to `state->unread_count = 0;`).

(d) In `ui_on_message_received`, refine the already-viewing branch from Task 2: a reader scrolled into history should get an unread count, not a silent repaint. Replace the first block with:

```c
    if (state->current_screen == SCREEN_MESSAGES && state->msg_scroll == 0) {
        if (state->message_auto_switch_time != 0) {
            state->message_auto_switch_time = now_ms;
        }
        state->screen_dirty = true;
        return;
    }
    if (state->current_screen == SCREEN_MESSAGES) {
        /* Scrolled into history: count it, do not yank the view. */
        if (state->unread_count < 99) {
            state->unread_count++;
        }
        state->screen_dirty = true;
        return;
    }
```

(e) In `ui_check_timeout`, gate the auto-restore on not being scrolled:

```c
    if (state->message_auto_switch_time != 0 && state->current_screen == SCREEN_MESSAGES &&
        state->msg_scroll == 0 &&
        (now_ms - state->message_auto_switch_time) >= UI_MESSAGE_AUTO_RESTORE_TIMEOUT_MS) {
```

- [ ] **Step 4: Run tests**

```bash
cd test/build && make test_ui >/dev/null && ./test_ui
```
Expected: PASS (including the pre-existing `test_user_interaction_on_messages_cancels_auto_restore`, which long-presses on messages: the top-of-function cancel still runs before the scroll block).

- [ ] **Step 5: Commit**

```bash
git add components/ui test/test_ui.c
git commit -m "feat(ui): scrollback on the messages screen

Long-press pages into history (clamped), double-press returns to
newest, and arrivals while scrolled count as unread instead of
yanking the view. Auto-restore is suppressed while scrolled."
```

---

### Task 5: Message line formatter (ui_format)

**Files:**
- Modify: `components/ui/include/ui.h`
- Modify: `components/ui/ui_format.c`
- Test: `test/test_ui.c`

**Interfaces:**
- Produces:

```c
typedef struct {
    const char* text;
    int text_len;
    bool outgoing;
    uint32_t peer_addr;
    const char* peer_name;    /* NULL or "" when unknown */
    int channel_index;        /* <= 0: default/none, no tag */
    const char* channel_name; /* NULL or "": fall back to "#<index>" */
    const char* badge;        /* "", " *", " +", "++", " x" (caller-computed) */
    int age_s;                /* seconds since stored; < 0 hides the age suffix */
} ui_msg_line_t;

int ui_format_msg_line(const ui_msg_line_t* m, char* buf, size_t buf_len);
```

Rendering rules: sender is `me` (outgoing), else `peer_name` truncated to 8, else `%04X` of the low 16 address bits. Non-default channels prefix `#name ` (name truncated to 4) or `#N `. CTCP ACTION text (`\x01ACTION ...\x01`) renders `* sender action`. Suffix: the delivery badge when non-empty, else an age suffix (` 5s` / ` 5m` / ` 3h`) when `age_s >= 0`. Text truncates so the suffix always fits within `buf_len - 1`. Task 6 calls this from the renderer.

- [ ] **Step 1: Write failing tests**

Add to `test/test_ui.c`:

```c
void test_format_msg_line_incoming_named_sender(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "hello there",
                       .text_len = 11,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ally: hello there", buf);
}

void test_format_msg_line_age_suffix(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "hello",
                       .text_len = 5,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = 300};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ally: hello 5m", buf);
}

void test_format_msg_line_unknown_sender_uses_hex(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "hi",
                       .text_len = 2,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = NULL,
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("C3D4: hi", buf);
}

void test_format_msg_line_outgoing_with_badge_and_truncation(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "a very long message that will not fit",
                       .text_len = 38,
                       .outgoing = true,
                       .peer_addr = 0,
                       .peer_name = NULL,
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = " +",
                       .age_s = -1};
    int n = ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n <= 21);
    /* used = "me" + ": " + " +" = 6, so 15 text chars fit on the 21-char line */
    TEST_ASSERT_EQUAL_STRING("me: a very long mes +", buf);
}

void test_format_msg_line_channel_tag(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "yo",
                       .text_len = 2,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 2,
                       .channel_name = "hiking",
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#hiki ally: yo", buf);
}

void test_format_msg_line_channel_tag_falls_back_to_index(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "yo",
                       .text_len = 2,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = NULL,
                       .channel_index = 3,
                       .channel_name = "",
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#3 C3D4: yo", buf);
}

void test_format_msg_line_action(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "\x01" "ACTION waves\x01",
                       .text_len = 14,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("* ally waves", buf);
}
```

Register all seven with `RUN_TEST`.

- [ ] **Step 2: Run to verify failure**

```bash
cd test/build && make test_ui 2>&1 | tail -3
```
Expected: compile FAIL (`ui_msg_line_t` unknown).

- [ ] **Step 3: Implement**

In `components/ui/include/ui.h`, after the existing formatter declarations:

```c
/* One rendered message line for the text (OLED) UI. All lookups
 * (beacon name, channel name, status badge) happen in the caller;
 * this stays a pure, host-testable formatter. */
typedef struct {
    const char* text;
    int text_len;
    bool outgoing;
    uint32_t peer_addr;
    const char* peer_name;    /* NULL or "" when unknown */
    int channel_index;        /* <= 0: default/none, no tag */
    const char* channel_name; /* NULL or "": fall back to "#<index>" */
    const char* badge;        /* "", " *", " +", "++", " x" */
} ui_msg_line_t;

int ui_format_msg_line(const ui_msg_line_t* m, char* buf, size_t buf_len);
```

In `components/ui/ui_format.c`, add:

```c
int ui_format_msg_line(const ui_msg_line_t* m, char* buf, size_t buf_len) {
    if (!m || !buf || buf_len == 0)
        return 0;

    char sender[10];
    if (m->outgoing) {
        snprintf(sender, sizeof(sender), "me");
    } else if (m->peer_name && m->peer_name[0]) {
        snprintf(sender, sizeof(sender), "%.8s", m->peer_name);
    } else {
        snprintf(sender, sizeof(sender), "%04X", (unsigned)(m->peer_addr & 0xFFFF));
    }

    const char* text = m->text ? m->text : "";
    int text_len = m->text_len;

    bool is_action =
        (text_len > 9 && text[0] == 0x01 && strncmp(text + 1, "ACTION ", 7) == 0);
    if (is_action) {
        const char* act = text + 8;
        int act_len = text_len - 8;
        if (act_len > 0 && act[act_len - 1] == 0x01)
            act_len--;
        if (act_len < 0)
            act_len = 0;
        return snprintf(buf, buf_len, "* %s %.*s", sender, act_len, act);
    }

    char tag[8] = "";
    if (m->channel_index > 0) {
        if (m->channel_name && m->channel_name[0])
            snprintf(tag, sizeof(tag), "#%.4s ", m->channel_name);
        else
            snprintf(tag, sizeof(tag), "#%d ", m->channel_index);
    }

    /* Suffix: the delivery badge wins; otherwise an age stamp when provided. */
    char suffix[8];
    if (m->badge && m->badge[0]) {
        snprintf(suffix, sizeof(suffix), "%s", m->badge);
    } else if (m->age_s >= 0) {
        if (m->age_s < 60)
            snprintf(suffix, sizeof(suffix), " %ds", m->age_s);
        else if (m->age_s < 3600)
            snprintf(suffix, sizeof(suffix), " %dm", m->age_s / 60);
        else
            snprintf(suffix, sizeof(suffix), " %dh", m->age_s / 3600);
    } else {
        suffix[0] = '\0';
    }

    int used = (int)strlen(tag) + (int)strlen(sender) + 2 + (int)strlen(suffix);
    int text_max = (int)buf_len - 1 - used;
    if (text_max < 1)
        text_max = 1;
    if (text_len > text_max)
        text_len = text_max;
    return snprintf(buf, buf_len, "%s%s: %.*s%s", tag, sender, text_len, text, suffix);
}
```

(`ui_format.c` already includes `ui.h`; add `#include <stdio.h>` and `#include <string.h>` if not present.)

- [ ] **Step 4: Run tests**

```bash
cd test/build && make test_ui >/dev/null && ./test_ui
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add components/ui test/test_ui.c
git commit -m "feat(ui): message line formatter with sender identity and channel tag

Pure formatter for the OLED message list: sender is the beacon name,
short hex address, or me; non-default channels get a #name tag; CTCP
ACTION renders as * sender action; text truncates so the delivery
badge always fits on the 21-char line."
```

---

### Task 6: Wire it all into the renderer (main.c)

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: `msg_store_total_incoming()` (Task 1), `ui_state_t.unread_count` (Task 2), `ui_set_message_total` + `msg_scroll` + `UI_MSG_PAGE_LINES` (Task 4), `ui_format_msg_line` (Task 5), plus existing `neighbor_lookup(&table, addr)` (`components/routing/include/routing.h:38`, entries carry `name[17]`) and `mesh_get_channel_name(int)` (`main/mesh_task.h:126`).
- No new host tests (this is display plumbing); the gate is the firmware build plus existing host suite.

- [ ] **Step 1: Fix the arrival detector**

In `app_main`'s loop (`main/main.c:1319-1326`), replace:

```c
        /* Incoming message detection for text UI boards. */
        {
            int current_message_count = msg_store_count();
            if (current_message_count > last_message_count) {
                ui_on_message_received(&ui, now_ms);
            }
            last_message_count = current_message_count;
        }
```

with:

```c
        /* Incoming message detection for text UI boards. Uses the monotonic
         * incoming counter: msg_store_count() saturates at the ring capacity
         * and outgoing sends must not trigger the notification path. */
        {
            uint32_t incoming_total = msg_store_total_incoming();
            if (incoming_total != last_incoming_total) {
                ui_on_message_received(&ui, now_ms);
                last_incoming_total = incoming_total;
            }
            ui_set_message_total(&ui, msg_store_count());
        }
```

And replace the `last_message_count` declaration above the loop (search for `last_message_count`) with:

```c
    uint32_t last_incoming_total = 0;
```

- [ ] **Step 2: Unread badge helper + calls**

Add above `render_main_screen` in `main/main.c`:

```c
/* "*N" unread badge, drawn right-aligned at the given x limit on every
 * screen except Messages itself. */
static void render_unread_badge(const ui_state_t* ui, int right_x) {
    if (ui->unread_count <= 0 || ui->current_screen == SCREEN_MESSAGES)
        return;
    char b[8];
    if (ui->unread_count > 9)
        snprintf(b, sizeof(b), "*9+");
    else
        snprintf(b, sizeof(b), "*%d", ui->unread_count);
    display_draw_text(right_x - (int)strlen(b) * FONT_W, HEADER_Y, b);
}
```

Change `render_main_screen(void)` to `render_main_screen(const ui_state_t* ui)` and update its two call sites (`render_screen`'s `case SCREEN_MAIN:` and the periodic refresh at `main.c:1402-1404`) to pass `&ui` / `ui`. Inside `render_main_screen`, after the battery text is drawn, add:

```c
        render_unread_badge(ui, batt_x - FONT_W);
```

(`batt_x` is already computed in that block; keep the badge inside the same scope.)

In `render_screen`, immediately after each header `display_draw_text(2, HEADER_Y, ...)` for `SCREEN_NODES`, `SCREEN_SETTINGS`, `SCREEN_GPS`, and the Heltec stats branch of `SCREEN_COMPOSE`, add:

```c
        render_unread_badge(ui, DISPLAY_WIDTH - 2);
```

- [ ] **Step 3: Rewrite the messages renderer**

Replace the body of `case SCREEN_MESSAGES:` in `render_screen` (`main.c:373-472`) with:

```c
    case SCREEN_MESSAGES: {
        display_clear();
        int mcount = msg_store_count();
        char hdr[32];
        if (ui->msg_scroll > 0)
            snprintf(hdr, sizeof(hdr), "Messages (%d) ^%d", mcount, ui->msg_scroll);
        else
            snprintf(hdr, sizeof(hdr), "Messages (%d)", mcount);
        display_draw_text(2, HEADER_Y, hdr);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        if (mcount == 0) {
            int no_msg_y = (DISPLAY_HEIGHT - FONT_H) / 2;
            const char* no_msg = "(no messages yet)";
            int no_msg_x = (DISPLAY_WIDTH - strlen(no_msg) * FONT_W) / 2;
            display_draw_text(no_msg_x, no_msg_y, no_msg);
        } else {
            mesh_get_state(&s_render_mesh); /* for beacon names */
            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            int max_msgs = (FOOTER_Y - CONTENT_Y) / LINE_H;
            int start = mcount - max_msgs - ui->msg_scroll;
            if (start < 0)
                start = 0;
            int y = CONTENT_Y;
            for (int i = start; i < mcount && y < FOOTER_Y; i++) {
                const stored_msg_t* m = msg_store_get(i);
                if (!m)
                    continue;

                bool outgoing =
                    (m->direction == MSG_DIR_OUTGOING || m->direction == MSG_DIR_BROADCAST_OUT);

                const char* badge = "";
                if (m->direction == MSG_DIR_OUTGOING) {
                    switch (m->status) {
                    case MSG_STATUS_SENT:
                        badge = " *";
                        break;
                    case MSG_STATUS_DELIVERED:
                        badge = (m->route_hop_count > 1) ? "++" : " +";
                        break;
                    case MSG_STATUS_FAILED:
                        badge = " x";
                        break;
                    default:
                        break;
                    }
                }

                const char* peer_name = NULL;
                if (!outgoing) {
                    neighbor_entry_t* nb =
                        neighbor_lookup(&s_render_mesh.neighbors, m->peer_addr);
                    if (nb && nb->name[0])
                        peer_name = nb->name;
                }

                ui_msg_line_t li = {
                    .text = m->text,
                    .text_len = m->text_len,
                    .outgoing = outgoing,
                    .peer_addr = m->peer_addr,
                    .peer_name = peer_name,
                    .channel_index = m->channel_index,
                    .channel_name =
                        (m->channel_index > 0) ? mesh_get_channel_name(m->channel_index) : NULL,
                    .badge = badge,
                    .age_s = (now_s >= m->timestamp_s) ? (int)(now_s - m->timestamp_s) : 0,
                };
                char line[CHARS_PER_LINE + 1];
                ui_format_msg_line(&li, line, sizeof(line));
                display_draw_text(2, y, line);
                y += LINE_H;
            }
        }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        display_draw_text(2, FOOTER_Y, "[o] compose  < > navigate");
#else
        if (ui->msg_scroll > 0)
            display_draw_text(2, FOOTER_Y, "[2x]new [hold]older");
        else
            display_draw_text(2, FOOTER_Y, "[hold]older reply:app");
#endif
        display_flush();
        break;
    }
```

(The CTCP ACTION handling moved into the formatter; the old inline block goes away entirely.)

- [ ] **Step 4: Nodes screen names, settings footer, stats legend**

(a) In `case SCREEN_NODES:` replace the line formatter (`main.c:534-535`):

```c
                /* Line: "AABBCCDD -70 12s" (~16 chars, fits 21-char display) */
                snprintf(nl, sizeof(nl), "%08" PRIX32 " %d %s", e->addr, e->rssi, age_str);
```

with:

```c
                /* Prefer the beacon name; fall back to the full address. */
                if (e->name[0])
                    snprintf(nl, sizeof(nl), "%.8s %d %s", e->name, e->rssi, age_str);
                else
                    snprintf(nl, sizeof(nl), "%08" PRIX32 " %d %s", e->addr, e->rssi, age_str);
```

(b) In the settings non-edit footer (`main.c` around line 640), replace:

```c
            display_draw_text(2, FOOTER_Y, "[press]next [hold]edit");
```

with:

```c
            display_draw_text(2, FOOTER_Y, "[hold]edit [2x]exit");
```

(short press cycles rows there, not screens; the old hint was wrong and hid the exit).

(c) In the Heltec stats branch of `case SCREEN_COMPOSE:` (after the last content line is drawn, before `display_draw_text(2, FOOTER_Y, ...)`), add a delivery-badge legend guarded by available space:

```c
        if (y <= FOOTER_Y - LINE_H) {
            display_draw_text(2, y, "*pend +ok ++mh x fail");
        }
```

- [ ] **Step 5: Build both Heltec boards and run host tests**

```bash
bash test/run_all_tests.sh
bash scripts/flash.sh local heltec-v3 build
bash scripts/flash.sh local heltec-v4 build
```
Expected: host suite all green; both firmware builds succeed. Fix any compile errors (likely candidates: `const` mismatch on `neighbor_lookup`, missing `#include` for `ui.h` types in main.c, the `render_main_screen` signature change).

- [ ] **Step 6: Commit**

```bash
git add main/main.c
git commit -m "feat(ui): wire OLED notification, identity, and scrollback into the renderer

Arrival detection now uses the monotonic incoming counter (survives the
20-slot ring, ignores outgoing). Unread badge on every screen header,
press-to-view footer hints, sender names and channel tags in the
message list, beacon names on the nodes screen, honest settings footer,
scrollback window, and a delivery-badge legend on the stats screen."
```

---

### Task 7: Full verification + PR

**Files:** none new (may include this plan file)

- [ ] **Step 1: Full local gate**

```bash
bash test/run_all_tests.sh
bash scripts/check-rpc-contract.sh
bash scripts/flash.sh local heltec-v3 build
bash scripts/flash.sh local heltec-v4 build
```
Expected: everything green.

- [ ] **Step 2: Self-review the diff**

```bash
git diff origin/main...HEAD --stat
git log --oneline origin/main..HEAD
```
Check: no em dashes introduced, no stray debug prints, commits are conventional.

- [ ] **Step 3: Push and open PR (Gitea API, not gh)**

```bash
git push -u origin feat/heltec-oled-ux
PAT=$(cat ~/src/bramble-meta/secrets/gitea-pat)
curl -sS -X POST -H "Authorization: token $PAT" -H "Content-Type: application/json" \
  "https://git.idiotica.org/api/v1/repos/dumbot/bramble/pulls" \
  -d '{"title": "feat(ui): Heltec OLED pager UX batch", "head": "feat/heltec-oled-ux", "base": "main", "body": "..."}'
```
PR body: summarize the two bug fixes and the feature list; test plan = host suite + both board builds + manual on-device checklist below.

- [ ] **Step 4: Wait for CI green** (poll `GET /repos/dumbot/bramble/commits/{sha}/statuses`), fix failures if any.

- [ ] **Step 5: Report PR URL and STOP.** Merging requires explicit user instruction (house rule). Hand the user this on-device checklist:

```
Manual on-device verification (V3 is FLASH-ENCRYPTED: flash with --encrypt or it bricks):
1. Flash V4 + V3, send >20 messages from the webapp to one node, verify
   the OLED still auto-switches on the 21st+ (ring-wrap regression).
2. Press a button, then send a message within 10 s: badge appears in the
   header, next short press jumps to Messages.
3. Verify sender names appear for beaconing neighbors, hex for others,
   channel tag on a non-default channel message.
4. Long-press to scroll history; double-press returns to newest.
5. Sit on Messages reading for >60 s: screen must NOT bounce to MAIN.
```
