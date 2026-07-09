# Building & Flashing Bramble

This doc is the source of truth for firmware build/flash workflows.

## Recommended workflow (board-aware wrapper)

Use `scripts/flash.sh` for all normal firmware builds and flashes. It applies the correct board defaults, build directory, and SDKCONFIG file.

```bash
# Heltec V3
bash scripts/flash.sh local heltec-v3 build
bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0

# Heltec V4
bash scripts/flash.sh local heltec-v4 build
bash scripts/flash.sh local heltec-v4 flash /dev/ttyUSB0

# T-Deck Plus
bash scripts/flash.sh local tdeck-plus build
bash scripts/flash.sh local tdeck-plus flash /dev/ttyACM0
```

Monitor serial output:

```bash
bash scripts/flash.sh local heltec-v3 monitor /dev/ttyUSB0
```

> Tip: run `bash scripts/flash.sh --help` for full argument details.

---

## Prerequisites

### ESP-IDF v5.4

```bash
git clone --depth 1 -b v5.4 https://github.com/espressif/esp-idf.git ~/src/esp-idf
cd ~/src/esp-idf
git submodule update --init --recursive --depth 1
./install.sh esp32s3
```

### Activate ESP-IDF in your shell

The wrapper expects `~/src/esp-idf/export.sh` to exist.

```bash
export IDF_PATH=~/src/esp-idf
source "$IDF_PATH/export.sh"
```

If your ESP-IDF Python venv is not auto-selected:

```bash
IDF_VENV=$(ls -d "$HOME/.espressif/python_env"/idf*.4_py*_env 2>/dev/null | sort -V | tail -1 || true)
if [[ -n "${IDF_VENV:-}" ]]; then
  export PATH="$IDF_VENV/bin:$PATH"
fi
```

---

## Build outputs

Per-board build directories:

- `build-heltec-v3/`
- `build-heltec-v4/`
- `build-tdeck-plus/`

Per-board sdkconfig files:

- `sdkconfig.heltec-v3`
- `sdkconfig.heltec-v4`
- `sdkconfig.tdeck-plus`

Main firmware artifact:

- `<board-build-dir>/bramble.bin`

---

## Advanced: direct `idf.py` usage (optional)

Use direct `idf.py` only when you explicitly need low-level control. Prefer the wrapper above for routine work.

```bash
# Example: Heltec V4 direct build
idf.py \
  -B build-heltec-v4 \
  -D SDKCONFIG=sdkconfig.heltec-v4 \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" \
  build
```

---

## Host-side tests (no hardware)

```bash
bash test/run_all_tests.sh
```

Webapp tests:

```bash
cd webapp
npm ci
npm test
```

---

## Supported hardware

| Board | MCU | Radio | Status |
|---|---|---|---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | SX1262 | ✅ Running target |
| Heltec WiFi LoRa 32 V4 | ESP32-S3 | SX1262 (+ optional L76K GNSS) | ✅ Running target (GNSS pin mapping validation ongoing) |
| LilyGo T-Deck Plus | ESP32-S3 | SX1262 | ✅ Running target with LVGL v9 GUI |

For historical V4 GNSS bring-up notes, see [archive/heltec-v4-gnss-bringup.md](archive/heltec-v4-gnss-bringup.md); GNSS pins now live in `main/boards/heltec_v4.h`.
