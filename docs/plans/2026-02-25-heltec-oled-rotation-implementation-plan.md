# Heltec OLED 180° Rotation Setting Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Add a user-accessible Settings option on Heltec OLED devices to rotate the screen 180° and persist that preference across reboots.

**Architecture:** Extend the existing non-graphical Settings UX (currently used for WiFi/BLE mode selection) to include a second setting for OLED orientation. Add a persisted display orientation flag in NVS, apply it during display init and at runtime when changed, and wire it into rendering + confirmation flow in `main.c`.

**Tech Stack:** ESP-IDF (C), SSD1306 display driver, existing Bramble UI state machine, Unity unit tests (`test/test_ui.c`).

---

### Task 1: Add display rotation API + SSD1306 implementation

**Files:**
- Modify: `components/display/include/display.h`
- Modify: `components/display/ssd1306.c`
- Modify: `components/display/st7789.c` (no-op compatibility implementation)

**Step 1: Add API in header**
- Add:
  - `void display_set_rotated_180(bool rotated);`
  - `bool display_get_rotated_180(void);`

**Step 2: Implement SSD1306 runtime rotation**
- In `ssd1306.c`, add static state: `s_rotated_180`.
- Add helper that sends orientation commands:
  - Normal: `0xA1` + `0xC8` (current orientation)
  - Rotated 180: `0xA0` + `0xC0`
- Call helper during `display_init()` before first flush.
- Implement `display_set_rotated_180()` to update state + send commands immediately when initialized.
- Implement `display_get_rotated_180()` returning the current state.

**Step 3: Keep ST7789 build compatibility**
- Implement same API in `st7789.c` as stateful no-op or mapped rotation as desired, but no behavior change for T-Deck required in this task.

**Step 4: Build/test sanity**
Run: `cmake --build build`
Expected: successful compile.

---

### Task 2: Extend Settings data model and button UX flow

**Files:**
- Modify: `components/ui/include/ui.h`
- Modify: `components/ui/ui_manager.c`
- Modify: `test/test_ui.c`

**Step 1: Add settings item model**
- Introduce settings item enum and count (e.g., `UI_SETTINGS_ITEM_CONN_MODE`, `UI_SETTINGS_ITEM_OLED_ROTATION`).
- Add fields in `ui_state_t`:
  - selected settings item cursor (menu-level)
  - existing edit cursor re-used for value selection where applicable

**Step 2: Update Settings interaction flow**
- Non-editing Settings screen:
  - Up/down cycles between setting rows
  - Select/long-press enters editing for selected row
- Editing mode:
  - For connectivity row: existing WiFi/BLE left-right or up-down cycling and confirm/cancel behavior preserved.
  - For OLED rotation row: options `Normal` and `UpsideDown(180)` with same confirm/cancel mechanics.

**Step 3: Update tests**
- Add/adjust unit tests in `test/test_ui.c` for:
  - navigating between settings rows
  - entering/exiting edit mode per selected row
  - value cursor behavior for new row
  - no regressions for existing connectivity behavior

**Step 4: Run tests**
Run: `ctest --test-dir build --output-on-failure -R test_ui`
Expected: all `test_ui` tests pass.

---

### Task 3: Persist/display/apply OLED rotation from Settings

**Files:**
- Modify: `main/main.c`

**Step 1: Add NVS storage functions**
- Add `oled_rotation_get()` / `oled_rotation_set()` (NVS key e.g., `oled_rot180`, namespace `bramble`).
- Default to `false` (normal orientation).

**Step 2: Apply rotation at boot**
- After `display_init()`, load persisted flag and call `display_set_rotated_180(...)`.

**Step 3: Render Settings rows + value**
- In non-editing Settings view, show:
  - Connectivity mode row
  - OLED orientation row with current value
- In editing view, show value picker for selected row, with existing footer hints adapted for both rows.

**Step 4: Confirm handling in main loop**
- In existing `ui.settings_confirmed` handling:
  - Branch by selected settings item.
  - For connectivity: retain reboot-required flow.
  - For OLED rotation: persist setting, call `display_set_rotated_180()` immediately, redraw without reboot.

**Step 5: Verification build + focused checks**
Run:
- `cmake --build build`
- `ctest --test-dir build --output-on-failure -R test_ui`
Expected: both pass.

---

### Task 4: Final verification + evidence notes

**Files:**
- Modify/Create: `docs/plans/evidence/2026-02-25-heltec-oled-rotation.md`

**Step 1: Record verification evidence**
- Document exact commands run and outputs (build + tests).
- Document Settings UX mapping (short/long/confirm/cancel) and new OLED row labels.

**Step 2: Optional hardware verification checklist**
- Add concise checklist for manual on-device verification:
  - Enter Settings
  - Select OLED orientation
  - Confirm rotate 180°
  - Reboot and verify persisted orientation

**Step 3: Commit**
Run:
- `git add ...`
- `git commit -m "feat(ui): add Heltec OLED 180-degree rotation setting"`

