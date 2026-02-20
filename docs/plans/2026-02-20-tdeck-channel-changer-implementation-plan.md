# T-Deck Plus Channel Changer Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Add a session-only channel changer in T-Deck Plus Chat UI with Broadcast as a special first option, and route sends/filtering by selected target.

**Architecture:** Add a small chat-target state model in `ui_graphics` (Broadcast vs channel index), expose a picker from Chat header, and thread that selection into chat detail send/filter logic. Keep persistence out of scope (no default channel writes). Add minimal mesh API support for sending on non-default channels and host tests for new filtering/selection helpers.

**Tech Stack:** ESP-IDF (C), LVGL v9, existing `ui_graphics` screens, `mesh_task`, Unity host tests (`test/`)

---

### Task 1: Add chat target model + pure helper functions (test-first)

**Files:**
- Create: `components/ui_graphics/include/chat_target.h`
- Create: `components/ui_graphics/chat_target.c`
- Create: `test/test_chat_target.c`
- Modify: `test/CMakeLists.txt`

**Step 1: Write the failing test**

Add tests in `test/test_chat_target.c` for:
- default target is Broadcast
- broadcast target matches only broadcast directions
- channel target matches only messages with same `channel_index`
- invalid channel index falls back to Broadcast in normalization helper

Example skeleton:
```c
void test_default_target_is_broadcast(void) {
    chat_target_t t = chat_target_default();
    TEST_ASSERT_EQUAL(CHAT_TARGET_BROADCAST, t.kind);
}
```

**Step 2: Run test to verify it fails**

Run:
```bash
cd /home/justin/src/bramble/test
mkdir -p build && cd build
cmake .. && make -j$(nproc)
./test_chat_target
```
Expected: FAIL (missing symbols/files).

**Step 3: Write minimal implementation**

Implement:
- enum + struct in `chat_target.h`
- `chat_target_default()`
- `chat_target_normalize(kind, channel_idx, channel_count)`
- `chat_target_matches_message(target, stored_msg_t*)`

**Step 4: Run test to verify it passes**

Run:
```bash
cd /home/justin/src/bramble/test/build
make -j$(nproc) test_chat_target
./test_chat_target
```
Expected: PASS.

**Step 5: Commit**

```bash
git -C /home/justin/src/bramble add \
  components/ui_graphics/include/chat_target.h \
  components/ui_graphics/chat_target.c \
  test/test_chat_target.c test/CMakeLists.txt
git -C /home/justin/src/bramble commit -m "test(ui): add chat target helper model with unit tests"
```

---

### Task 2: Extend message model for channel-aware filtering (test-first)

**Files:**
- Modify: `components/msg_store/include/msg_store.h`
- Modify: `components/msg_store/msg_store.c`
- Modify: `test/test_msg_store.c` (create if missing)
- Modify: `test/CMakeLists.txt`

**Step 1: Write the failing test**

Add tests proving stored messages retain `channel_index` and default to `-1` for broadcast/non-channel entries.

**Step 2: Run test to verify it fails**

Run:
```bash
cd /home/justin/src/bramble/test/build
make -j$(nproc) test_msg_store
./test_msg_store
```
Expected: FAIL (field/function not present).

**Step 3: Write minimal implementation**

- Add `int16_t channel_index;` to `stored_msg_t`.
- Add `msg_store_add_ex2(..., int16_t channel_index)` (or equivalent minimal API) while keeping backward-compatible wrappers.
- Ensure old call sites map to `channel_index = -1`.

**Step 4: Run tests to verify pass**

Run:
```bash
cd /home/justin/src/bramble/test/build
make -j$(nproc) test_msg_store
./test_msg_store
```
Expected: PASS.

**Step 5: Commit**

```bash
git -C /home/justin/src/bramble add \
  components/msg_store/include/msg_store.h \
  components/msg_store/msg_store.c \
  test/test_msg_store.c test/CMakeLists.txt
git -C /home/justin/src/bramble commit -m "feat(msg_store): track channel index for chat filtering"
```

---

### Task 3: Add mesh API for channel-specific send + persist channel metadata in RX/TX paths

**Files:**
- Modify: `main/mesh_task.h`
- Modify: `main/mesh_task.c`
- Modify: `components/msg_store/msg_store.c` call sites in mesh task integration
- Test: `test/test_channel_msg.c` (augment) and/or `test/test_chat_target.c`

**Step 1: Write failing tests (or assertions in existing tests)**

Add/extend tests for:
- invalid channel index rejected
- broadcast path unchanged
- channel send path uses selected channel index metadata for stored outgoing message

**Step 2: Run tests to verify failure**

Run:
```bash
cd /home/justin/src/bramble/test/build
make -j$(nproc) test_channel_msg test_chat_target
./test_channel_msg
./test_chat_target
```
Expected: at least one FAIL due to missing API/behavior.

**Step 3: Write minimal implementation**

- Add in header:
```c
int mesh_send_channel(int channel_idx, const uint8_t *data, size_t len);
```
- In `mesh_task.c`, implement guarded channel send using `s_channels[channel_idx]`.
- Keep `mesh_send_broadcast()` behavior/rate-limiting unchanged.
- Ensure incoming decrypted channel messages store `channel_index = info.channel_id`.

**Step 4: Run tests to verify pass**

Run same commands as Step 2; expected PASS.

**Step 5: Commit**

```bash
git -C /home/justin/src/bramble add main/mesh_task.h main/mesh_task.c
git -C /home/justin/src/bramble commit -m "feat(mesh): add channel-index send API for UI channel changer"
```

---

### Task 4: Implement channel picker in chat list header (Broadcast-first)

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_list.c`
- Modify: `components/ui_graphics/screens/scr_chat_list.h`
- Modify: `components/ui_graphics/CMakeLists.txt` (if new source files added)
- Optional Create: `components/ui_graphics/screens/scr_channel_picker.c`
- Optional Create: `components/ui_graphics/screens/scr_channel_picker.h`

**Step 1: Write failing integration check**

Add a lightweight host-level test for helper that builds picker label text:
- Broadcast renders `📢 Broadcast`
- Channel renders `#<name>`

**Step 2: Run test to verify failure**

Run relevant test binary (`test_chat_target` or new `test_ui_graphics_helpers`) and verify FAIL.

**Step 3: Write minimal implementation**

- Replace static `"Messages"` header with target chip button.
- On tap, open modal list with first item `📢 Broadcast` then channels from current config.
- Selection updates in-memory `chat_target_t` and rebuilds chat list.
- If selected channel no longer exists, normalize/fallback to Broadcast.

**Step 4: Run tests/build sanity**

Run:
```bash
cd /home/justin/src/bramble/test/build
make -j$(nproc) test_chat_target
./test_chat_target
```
Then firmware build:
```bash
cd /home/justin/src/bramble
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build
```
Expected: PASS/BUILD OK.

**Step 5: Commit**

```bash
git -C /home/justin/src/bramble add components/ui_graphics/screens/scr_chat_list.c components/ui_graphics/screens/scr_chat_list.h components/ui_graphics/CMakeLists.txt
# Include picker files if created
git -C /home/justin/src/bramble commit -m "feat(ui): add Broadcast-first channel picker in chat header"
```

---

### Task 5: Wire chat detail view send/filter to selected target

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_messages.c`
- Modify: `components/ui_graphics/screens/scr_chat_messages.h`
- Modify: `components/ui_graphics/screens/scr_chat_list.c` (open detail with target)

**Step 1: Write failing test/helper assertions**

Cover helper routing function:
- Broadcast target -> calls broadcast path
- Channel target -> calls channel send path with correct index
- send failure keeps compose text

**Step 2: Run tests and verify failure**

Run test binary, expect FAIL due to old hardcoded broadcast path.

**Step 3: Implement minimal code**

- Replace `int channel_idx` argument with `chat_target_t target` where practical.
- In `send_click_cb`, dispatch:
  - Broadcast: `mesh_send_broadcast(...)`
  - Channel: `mesh_send_channel(target.channel_idx, ...)`
- Filter displayed messages by `chat_target_matches_message(...)`.
- Keep back-navigation/tab-hide behavior unchanged.

**Step 4: Verify pass + build**

Run host tests and T-Deck build command again; expected green.

**Step 5: Commit**

```bash
git -C /home/justin/src/bramble add components/ui_graphics/screens/scr_chat_messages.c components/ui_graphics/screens/scr_chat_messages.h components/ui_graphics/screens/scr_chat_list.c
git -C /home/justin/src/bramble commit -m "feat(chat): route sends and message view by selected chat target"
```

---

### Task 6: End-to-end verification on hardware + docs update

**Files:**
- Modify: `reports/t-deck-plus-ui-documentation.md` (or source doc in bramble if preferred)
- Modify: `docs/plans/2026-02-20-tdeck-channel-changer-implementation-plan.md` (checkboxes/results)

**Step 1: Flash and smoke test**

Run:
```bash
cd /home/justin/src/bramble
sg dialout -c "idf.py -p /dev/ttyACM0 flash monitor"
```
Manual checks:
- Picker shows `📢 Broadcast` first
- Selecting channel changes header label
- Channel send arrives only in matching channel view
- Reboot resets selection to Broadcast
- Invalid/missing channel auto-falls back to Broadcast

**Step 2: Capture evidence**

Collect concise logs/screenshots for:
- selected target change
- send path log lines (broadcast vs channel index)
- fallback behavior

**Step 3: Update docs**

Document new behavior and known limits (session-only selection, no add/remove UI in this pass).

**Step 4: Final verification commands**

```bash
cd /home/justin/src/bramble/test/build
ctest --output-on-failure
cd /home/justin/src/bramble
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build
```
Expected: all host tests + firmware build pass.

**Step 5: Commit**

```bash
git -C /home/justin/src/bramble add reports/t-deck-plus-ui-documentation.md docs/plans/2026-02-20-tdeck-channel-changer-implementation-plan.md
git -C /home/justin/src/bramble commit -m "docs: document T-Deck chat target channel changer behavior"
```

---

## Notes / Guardrails

- Keep YAGNI: no on-device channel CRUD in this plan.
- Do not call `setDefaultChannel` from T-Deck UI for this feature.
- Preserve LVGL safety around flex layout cleanups (`lv_refr_now(...)` before problematic `lv_obj_clean(...)` paths).
- Broadcast remains a special public path and first picker option.
- Session-only state: do not persist selected target to NVS.
