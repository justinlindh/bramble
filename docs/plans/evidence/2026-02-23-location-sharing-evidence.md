# Location Sharing Verification Evidence (Task 9)

Date: 2026-02-23
Branch: `feature/location-sharing-impl-2026-02-23`
Repo: `/home/justin/src/bramble`

## Environment used (canonical ESP-IDF flow)

```bash
export IDF_PATH=~/src/esp-idf
IDF_VENV=$(ls -d "$HOME/.espressif/python_env"/idf*.4_py*_env 2>/dev/null | sort -V | tail -1 || true)
if [[ -n "${IDF_VENV:-}" && -x "$IDF_VENV/bin/python3" ]]; then
  export PATH="$IDF_VENV/bin:$PATH"
fi
source "$IDF_PATH/export.sh"
```

Resolved Python/toolchain during runs:
- Python: `3.14.2`
- ESP-IDF: `5.4`
- Venv: `~/.espressif/python_env/idf5.4_py3.14_env`

## Verification matrix

### 1) `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" build`
- Result: ✅ PASS
- Key output:
  - `Project build complete`
  - `bramble.bin binary size 0x1b51b0 bytes ... 0x20ae50 bytes (54%) free`
- Notes:
  - Compiler warnings seen for currently unused functions/vars in `main.c` and `mesh_task.c`.

### 2) `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build`
- Result: ✅ PASS
- Key output:
  - `Project build complete`
  - `bramble.bin binary size 0x1b51b0 bytes ... 0x20ae50 bytes (54%) free`

### 3) `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build`
- Result: ✅ PASS
- Key output:
  - `Project build complete`
  - `bramble.bin binary size 0x1b51b0 bytes ... 0x20ae50 bytes (54%) free`
- Notes:
  - Kconfig migration notice observed: `CONFIG_ESP32S3_SPIRAM_SUPPORT was replaced with CONFIG_SPIRAM`.

### 4) `bash test/run_all_tests.sh`
- Result: ✅ PASS
- Key output:
  - `Test suites: 53 total, 53 passed, 0 failed`
  - `ALL TESTS PASSED ✓`

### 5) `cd webapp && npm test`
- Result: ✅ PASS
- Key output:
  - `Test Files  26 passed (26)`
  - `Tests  107 passed (107)`
  - `Duration  2.56s`
- Notes:
  - Non-fatal deprecation warning observed: Vite CJS Node API deprecation message.

## Privacy/default checks status

- Fresh default sharing OFF: **Pending hardware validation**
- Default tier coarse on first enable: **Pending hardware validation**
- Disabled => no outbound location packets: **Pending hardware validation**

## Hardware acceptance matrix status

- T-Deck GPS fix + periodic location sends when enabled: **Pending (not run in this task execution)**
- Heltec (manual source) location sharing behavior: **Pending (not run in this task execution)**
- Reboot persistence checks on both device classes: **Pending (not run in this task execution)**

No hardware claims are made in this evidence file.
