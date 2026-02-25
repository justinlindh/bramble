# Heltec OLED Rotation — Task 4 Verification Evidence

Date: 2026-02-24 (PST)
Plan: `docs/plans/2026-02-25-heltec-oled-rotation-implementation-plan.md`

## 1) Verification commands and outputs

### A. Firmware build (as requested)
Command:
```bash
cd /home/justin/src/bramble
cmake --build build
```
Output:
```text
[0/1] Re-running CMake...
CMake Error at CMakeLists.txt:3 (include):
  include could not find requested file:

    /tools/cmake/project.cmake


CMake Error at /home/justin/src/esp-idf/tools/cmake/toolchain-esp32s3.cmake:1 (include):
  include could not find requested file:

    /tools/cmake/utilities.cmake
Call Stack (most recent call first):
  build/CMakeFiles/3.30.2/CMakeSystem.cmake:6 (include)
  CMakeLists.txt:4 (project)


CMake Error at /home/justin/src/esp-idf/tools/cmake/toolchain-esp32s3.cmake:10 (remove_duplicated_flags):
  Unknown CMake command "remove_duplicated_flags".
Call Stack (most recent call first):
  build/CMakeFiles/3.30.2/CMakeSystem.cmake:6 (include)
  CMakeLists.txt:4 (project)


-- Configuring incomplete, errors occurred!
FAILED: build.ninja
/home/justin/.espressif/tools/cmake/3.30.2/bin/cmake --regenerate-during-build -S/home/justin/src/bramble -B/home/justin/src/bramble/build
ninja: error: rebuilding 'build.ninja': subcommand failed
```
Result: **FAIL** (local ESP-IDF/CMake environment path issue; project include paths resolved as `/tools/...` instead of `${IDF_PATH}/tools/...`).

### B. Requested `ctest` invocation from repository `build/`
Command:
```bash
cd /home/justin/src/bramble
ctest --test-dir build --output-on-failure -R test_ui
```
Output:
```text
Test project /home/justin/src/bramble/build
No tests were found!!!
```
Result: **No test registrations** in firmware `build/` tree.

### C. Unit-test target rebuild (host test tree)
Command:
```bash
cd /home/justin/src/bramble
cmake --build test/build --target test_ui
```
Output:
```text
[100%] Built target test_ui
```
Result: **PASS**.

### D. Direct `test_ui` execution (host unit tests)
Command:
```bash
cd /home/justin/src/bramble/test/build
./test_ui
```
Output (summary):
```text
-----------------------
29 Tests 0 Failures 0 Ignored
OK
```
Result: **PASS** (`test_ui` executable, 29/29 passing).

---

## 2) Settings UX mapping (Heltec OLED rotation flow)

From the Task 1/2 code path (`components/ui/include/ui.h`, `components/ui/ui_manager.c`, `test/test_ui.c`):

- **Settings rows**
  - Row 1: Connectivity mode (`UI_SETTINGS_ITEM_CONN_MODE`)
  - Row 2: OLED orientation (`UI_SETTINGS_ITEM_OLED_ROTATION`)

- **Non-edit mode (row navigation)**
  - `UP`/`DOWN`: move between rows
  - `SELECT` or `LONG_PRESS`: enter edit mode on selected row

- **Edit mode (value selection)**
  - `UP`: previous value
  - `DOWN`: next value
  - `SELECT`/`LONG_PRESS`: confirm (`settings_confirmed = true`)
  - `LEFT`/`DOUBLE_PRESS`: cancel/back (exit edit mode)

- **OLED orientation value labels (for display render path)**
  - `Normal`
  - `UpsideDown (180°)`

---

## 3) Manual hardware verification checklist (optional)

1. Boot Heltec device and enter **Settings** screen.
2. Move cursor to **OLED orientation** row.
3. Enter edit mode; choose **UpsideDown (180°)**; confirm.
4. Verify display rotates immediately.
5. Reboot device.
6. Verify rotated orientation persists after reboot.
7. Repeat by selecting **Normal** and confirm persistence again.

---

## 4) Notes

- Task 4 evidence collected and recorded.
- Firmware build validation remains blocked by local ESP-IDF/CMake environment path/toolchain setup.
- Host-side `test_ui` binary rebuild and execution succeeded with all tests passing.
