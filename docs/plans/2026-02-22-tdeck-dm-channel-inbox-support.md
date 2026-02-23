# T-Deck Plus DM Channel Inbox Support Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Make incoming DMs on T-Deck Plus appear in the correct channel inbox (including Channel 0/Bramble Common), with clear DM/Broadcast labeling and per-channel unread badges.

**Architecture:** Keep send semantics unchanged (Broadcast sends remain broadcast). Refactor chat target matching to be channel-context-driven, then layer message-type labels in message view and unread aggregation in the chat list. Implement with TDD-first host tests, then firmware build/flash verification.

**Tech Stack:** ESP-IDF C firmware, LVGL UI (`ui_graphics`), Unity host tests.

---

### Task 1: Fix root matching logic for channel-context inbox rendering

**Files:**
- Modify: `components/ui_graphics/chat_target.c`
- Modify: `test/test_chat_target.c`
- Test: `test/CMakeLists.txt` (only if new test file registration is needed)

**Step 1: Write the failing tests**

Add/adjust tests in `test/test_chat_target.c` to codify channel-context rules:

```c
void test_broadcast_target_includes_channel0_unicast_dm(void) {
    chat_target_t t = chat_target_default();
    stored_msg_t dm_ch0_in = { .direction = MSG_DIR_INCOMING, .channel_index = 0 };
    stored_msg_t dm_ch0_out = { .direction = MSG_DIR_OUTGOING, .channel_index = 0 };

    TEST_ASSERT_TRUE(chat_target_matches_message(t, &dm_ch0_in, 0));
    TEST_ASSERT_TRUE(chat_target_matches_message(t, &dm_ch0_out, 0));
}

void test_broadcast_target_excludes_nonzero_channel_unicast_dm(void) {
    chat_target_t t = chat_target_default();
    stored_msg_t dm_ch2 = { .direction = MSG_DIR_INCOMING, .channel_index = 2 };

    TEST_ASSERT_FALSE(chat_target_matches_message(t, &dm_ch2, 2));
}

void test_channel_target_excludes_other_channel_dms(void) {
    chat_target_t t = chat_target_normalize(CHAT_TARGET_CHANNEL, 2, 4);
    stored_msg_t dm_ch1 = { .direction = MSG_DIR_INCOMING, .channel_index = 1 };

    TEST_ASSERT_FALSE(chat_target_matches_message(t, &dm_ch1, 1));
}
```

**Step 2: Run test to verify it fails**

Run:
```bash
cd ~/src/bramble/test
cmake -S . -B build
cmake --build build --target test_chat_target -j
./build/test_chat_target
```

Expected: FAIL in at least the new Channel 0 DM assertion(s).

**Step 3: Write minimal implementation**

Update `chat_target_matches_message(...)` in `components/ui_graphics/chat_target.c`:

```c
bool chat_target_matches_message(chat_target_t target,
                                 const stored_msg_t *msg,
                                 int message_channel_index) {
    if (!msg) return false;

    bool is_broadcast = (msg->direction == MSG_DIR_BROADCAST_IN ||
                         msg->direction == MSG_DIR_BROADCAST_OUT);

    if (target.kind == CHAT_TARGET_BROADCAST) {
        if (is_broadcast) return true;
        return message_channel_index == 0;
    }

    if (is_broadcast) {
        return target.channel_index == 0;
    }

    return message_channel_index == target.channel_index;
}
```

(If keeping Channel-0 as the special Broadcast target only, preserve current target cycling behavior and only adapt matching logic.)

**Step 4: Run test to verify it passes**

Run:
```bash
cd ~/src/bramble/test
cmake --build build --target test_chat_target -j
./build/test_chat_target
```

Expected: PASS all `test_chat_target` cases.

**Step 5: Commit**

```bash
git add components/ui_graphics/chat_target.c test/test_chat_target.c
git commit -m "fix(ui): show channel-0 DMs in broadcast inbox"
```

---

### Task 2: Add explicit DM vs Broadcast labeling in message bubbles

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_messages.c`
- Test: `test/test_chat_target.c` (optional helper tests if extraction helpers added)

**Step 1: Write failing test (helper-level) or define deterministic helper behavior**

If practical, extract tiny helpers in `scr_chat_messages.c` (or a new small helper file) and test:

```c
static bool is_broadcast_direction(msg_direction_t d);
static const char *message_kind_label(const stored_msg_t *m);
```

Expected mapping:
- `MSG_DIR_BROADCAST_IN/OUT` -> `"Broadcast"`
- `MSG_DIR_INCOMING/OUTGOING` -> `"DM"`

**Step 2: Run test to verify it fails**

Run relevant host test target (new or existing).

**Step 3: Implement minimal UI label rendering**

In `add_message_bubble(...)`, add a metadata line for type label:

```c
lv_obj_t *meta_lbl = lv_label_create(bubble);
lv_label_set_text(meta_lbl, is_broadcast ? "Broadcast" : "DM");
lv_obj_set_style_text_font(meta_lbl, &lv_font_montserrat_10, 0);
lv_obj_set_style_text_color(meta_lbl, BR_COLOR_TEXT_SEC, 0);
```

Determine `is_broadcast` from `msg->direction` in `render_messages_for_target()` and pass it into `add_message_bubble(...)`.

**Step 4: Verify visually and by build**

Run:
```bash
cd ~/src/bramble
idf.py build
```

Expected: Build success; chat bubbles now show DM/Broadcast context.

**Step 5: Commit**

```bash
git add components/ui_graphics/screens/scr_chat_messages.c
git commit -m "feat(ui): label chat entries as DM or Broadcast"
```

---

### Task 3: Add per-channel unread badge/count in chat list

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_list.c`
- Modify: `components/ui_graphics/ui_graphics.c`
- Modify: `components/ui_graphics/screens/scr_chat_messages.c`
- (Optional) Create: `components/ui_graphics/include/chat_unread.h`
- (Optional) Create: `components/ui_graphics/chat_unread.c`
- (Optional) Create: `test/test_chat_unread.c`
- Modify: `test/CMakeLists.txt` (if new test target)

**Step 1: Write failing tests for unread aggregation (recommended helper module)**

Create a tiny pure-C helper and test:

```c
void chat_unread_mark_for_message(const stored_msg_t *msg);
int chat_unread_count_for_channel(int channel_idx); // channel_idx 0 = broadcast/common
void chat_unread_clear_for_channel(int channel_idx);
```

Rules:
- Incoming broadcast -> increments channel 0
- Incoming DM on channel N -> increments N
- Outgoing messages do not increment unread

**Step 2: Run tests to verify fail**

Run:
```bash
cd ~/src/bramble/test
cmake --build build --target test_chat_unread -j
./build/test_chat_unread
```

Expected: FAIL before implementation.

**Step 3: Implement unread state + UI wiring**

- In `ui_graphics_notify(UI_EVT_MSG_RECEIVED)` processing path, update unread store based on newest message.
- In `scr_chat_list_create(...)`, for each channel card show right-aligned unread badge when count > 0.
- In `scr_chat_messages_open(layout, channel_idx)`, clear unread for that channel when opening.

Badge sketch:

```c
if (unread > 0) {
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_set_size(badge, 22, 18);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(badge, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(badge, 9, 0);

    lv_obj_t *badge_lbl = lv_label_create(badge);
    lv_label_set_text_fmt(badge_lbl, "%d", unread);
    lv_obj_center(badge_lbl);
}
```

**Step 4: Run tests + build**

Run:
```bash
cd ~/src/bramble/test
cmake --build build --target test_chat_target test_chat_unread -j
./build/test_chat_target
./build/test_chat_unread

cd ~/src/bramble
idf.py build
```

Expected: all relevant tests pass; firmware build passes.

**Step 5: Commit**

```bash
git add components/ui_graphics/screens/scr_chat_list.c \
        components/ui_graphics/ui_graphics.c \
        components/ui_graphics/screens/scr_chat_messages.c \
        components/ui_graphics/include/chat_unread.h \
        components/ui_graphics/chat_unread.c \
        test/test_chat_unread.c test/CMakeLists.txt
git commit -m "feat(ui): add per-channel unread badges in chat list"
```

---

### Task 4: End-to-end verification on T-Deck Plus hardware

**Files:**
- Modify: `memory/bramble-status.md` (session handoff updates)

**Step 1: Flash and run device**

```bash
cd ~/src/bramble
scripts/bramble-build.sh tdeck
```

Expected: successful build+flash.

**Step 2: Manual verification script**

1. Send DM to T-Deck Plus on channel 0 from another node.
2. Confirm device ping occurs.
3. Open Chat tab and verify:
   - Channel/Broadcast row now shows unread badge.
   - Opening row shows new DM bubble.
   - Bubble type label displays `DM`.
4. Send broadcast on channel 0 and verify bubble label `Broadcast` in same conversation.
5. Repeat DM test on channel 1/2 and verify appears only in correct channel row + view.
6. Open each channel and verify unread clears only for that channel.

**Step 3: Record evidence**

Capture serial logs and brief screenshots/photos (if available) proving:
- on receive tone still works
- row creation/visibility in correct channel
- unread clear behavior

**Step 4: Update status memory**

Append completed work and any caveats to `memory/bramble-status.md`.

**Step 5: Commit**

```bash
git add memory/bramble-status.md
git commit -m "docs(bramble): record DM inbox + unread badge verification"
```

---

### Task 5: Regression/guardrail pass before merge

**Files:**
- Modify: `docs/plans/2026-02-22-tdeck-dm-channel-inbox-support.md` (checklist completion notes)

**Step 1: Run full relevant tests**

```bash
cd ~/src/bramble/test
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Step 2: Firmware compile sanity**

```bash
cd ~/src/bramble
idf.py build
```

**Step 3: Confirm no behavior regressions**

Spot-check:
- Message send path for broadcast unchanged
- Channel cycle behavior unchanged
- Chat tab refresh still works with incoming events

**Step 4: Final commit (if checklist/doc updated)**

```bash
git add docs/plans/2026-02-22-tdeck-dm-channel-inbox-support.md
git commit -m "chore(plan): mark DM channel inbox implementation verification complete"
```

**Step 5: Prepare PR summary bullets**

Include:
- Root-cause fix (channel-aware matching)
- DM/Broadcast labeling
- Per-channel unread badges
- Test evidence + hardware validation

---

Plan complete and saved to `docs/plans/2026-02-22-tdeck-dm-channel-inbox-support.md`. Two execution options:

**1. Subagent-Driven (this session)** - I dispatch fresh subagent per task, review between tasks, fast iteration

**2. Parallel Session (separate)** - Open new session with executing-plans, batch execution with checkpoints

Which approach?