# Building & Flashing Bramble

This doc is the source of truth for firmware build/flash workflows.

If you do not want to install a toolchain, skip to
[Building in the container](#building-in-the-container-no-toolchain-install):
the repo ships the same ESP-IDF image CI uses, and it builds firmware with
Docker as the only host prerequisite.

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

# Bramble Pager v1 (custom PCB; native USB-Serial-JTAG console over USB-C)
bash scripts/flash.sh local bramble-pager build
bash scripts/flash.sh local bramble-pager flash /dev/ttyACM0
```

Monitor serial output:

```bash
bash scripts/flash.sh local heltec-v3 monitor /dev/ttyUSB0
```

> Tip: run `bash scripts/flash.sh --help` for full argument details.

---

## Prerequisites

### ESP-IDF v5.4.1

Bramble pins the exact ESP-IDF tag CI builds with. The pin lives in
`.esp-idf-version` at the repo root and is the single source of truth;
`scripts/lint/check-idf-version.sh` fails CI if any reference drifts from it.
Note that `v5.4.1` is a tag, not the moving `v5.4` release branch: cloning the
branch gets you a different tree than CI compiles with.

Clone ESP-IDF wherever you keep source. The examples below use
`$HOME/esp/esp-idf`; substitute your own location.

```bash
git clone --depth 1 -b v5.4.1 https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf"
cd "$HOME/esp/esp-idf"
git submodule update --init --recursive --depth 1
./install.sh esp32s3
```

> `install.sh esp32s3` installs the toolchain for real boards. The
> [emulator](../emulator/README.md) builds the same firmware for ESP-IDF's
> **linux** target, which is a separate install: run `./install.sh linux` as
> well if you plan to use it. Installing both is fine.

### Activate ESP-IDF in your shell

`scripts/flash.sh` locates ESP-IDF by checking `$IDF_PATH` first, then
`~/src/esp-idf`, `~/esp-idf`, `/opt/esp/idf`, and `/opt/esp-idf`. Setting
`IDF_PATH` explicitly is the reliable option:

```bash
export IDF_PATH="$HOME/esp/esp-idf"
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
- `build-bramble-pager/`

Per-board sdkconfig files:

- `sdkconfig.heltec-v3`
- `sdkconfig.heltec-v4`
- `sdkconfig.tdeck-plus`
- `sdkconfig.bramble-pager`

Main firmware artifact:

- `<board-build-dir>/bramble.bin`

---

## Building in the container (no toolchain install)

`docker/firmware-builder/Dockerfile` is the image Bramble's firmware CI runs
on. It is a normal container, so you can use it locally and skip installing
ESP-IDF entirely. Docker is the only host prerequisite.

Build the image once from the root of your checkout:

```bash
docker build -t bramble/idf-node:v5.4.1 docker/firmware-builder
```

Then build firmware by mounting your checkout at `/workspace`:

```bash
docker run --rm \
  -v "$PWD":/workspace -w /workspace \
  --user "$(id -u):$(id -g)" \
  bramble/idf-node:v5.4.1 \
  bash -lc 'source $IDF_PATH/export.sh >/dev/null && bash scripts/flash.sh local heltec-v3 build'
```

Swap `heltec-v3` for any board name from the wrapper section above. The
artifacts land in `build-<board>/` in your checkout exactly as a host build
would, so `build-heltec-v3/bramble.bin` is ready to flash.

Two things worth knowing:

- **Pass `--user`.** Without it the container runs as root and leaves
  root-owned `build-<board>/`, `sdkconfig.<board>`, and
  `keys/ota_signing_key.pem` files in your working tree. The signing key is
  generated mode `0600`, so a later non-root build cannot read it and fails
  with "Secure Boot Signing Key keys/ota_signing_key.pem does not exist". If
  you hit that, delete the root-owned key and build directory and start again
  with `--user`.
- **Building is not flashing.** This path covers build only. Flashing needs
  the serial device passed into the container, which is not covered here; run
  `bash scripts/flash.sh local <board> flash <port>` on the host against the
  artifacts the container produced.

The image contents, its version pins, and how it is published for the CI
runner fleet are documented in
[ci/idf-node-runner-image.md](ci/idf-node-runner-image.md).

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
| --- | --- | --- | --- |
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | SX1262 | ✅ Running target |
| Heltec WiFi LoRa 32 V4 | ESP32-S3 | SX1262 (+ optional L76K GNSS) | ✅ Running target (GNSS pin mapping validation ongoing) |
| LilyGo T-Deck Plus | ESP32-S3 | SX1262 | ✅ Running target with LVGL v9 GUI |
| Bramble Pager v1 (custom PCB) | ESP32-S3-WROOM-1 | SX1262 (DIO2 RF switch, TCXO) + ATGM336H GNSS | Design complete, boards not yet ordered |
| Seeed Wio-WM1110 Dev Kit | nRF52840 | LR1110 | 🚧 P2: joins the bench mesh, BLE RPC + provisioning over an encrypted link, identity/key/bonds persist in flash; GNSS and power management are P3; see [../nrf/README.md](../nrf/README.md) |

GNSS pins live in `main/boards/heltec_v4.h`. The nRF52840 target is a
separate bare-metal build (no ESP-IDF) under `nrf/`, with its own build and
flash instructions in [../nrf/README.md](../nrf/README.md).

### Bramble Pager v1 (custom board)

The Bramble Pager v1 is an in-house PCB design, not an off-the-shelf dev kit. Its board profile is selected by `CONFIG_BRAMBLE_BOARD_PAGER=y` from `sdkconfig.defaults.bramble_pager`, which also sets 8MB flash and a native USB-Serial-JTAG console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`): there is no UART bridge chip, so flash and monitor over the native USB-C port (it enumerates as `/dev/ttyACM*`, not `/dev/ttyUSB*`). The pin map is `main/boards/bramble_pager.h`. The full hardware design (spec, schematic, PCB, BOM, enclosure, and the pre-fab bring-up gates) lives in [../hardware/pager/v1/](../hardware/pager/v1/).

---

## Troubleshooting

Serial permissions on Linux, missing toolchains, the `esp32s3` versus `linux`
target distinction, and port collisions are covered in
[troubleshooting.md](troubleshooting.md).
