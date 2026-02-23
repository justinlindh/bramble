# Building & Flashing Bramble

## Prerequisites

### ESP-IDF v5.4

```bash
# Clone (without recursive — faster initial download)
git clone --depth 1 -b v5.4 https://github.com/espressif/esp-idf.git ~/src/esp-idf

# Init submodules (shallow, required for build)
cd ~/src/esp-idf
git submodule update --init --recursive --depth 1

# Install toolchain for ESP32-S3
./install.sh esp32s3

# If you hit SSL errors (common on some systems):
pip install --upgrade certifi
export SSL_CERT_FILE=$(python3 -c "import certifi; print(certifi.where())")
# Then retry install.sh
```

### System Dependencies

- **Python 3.10+** (tested with 3.12)
- **CMake 3.16+**
- **Ninja** (build system)
- **Git**
- **pip** (for ESP-IDF Python packages)
- **esptool** (installed by ESP-IDF, or `pip install esptool`)

### Known Issues

- **SSL certificate errors during install:** Run `pip install --upgrade certifi` and export `SSL_CERT_FILE` as shown above.
- **Missing submodules:** If cmake fails with "Missing X submodule", run `git submodule update --init --recursive --depth 1` in the ESP-IDF directory.
- **Flash size warning:** The Heltec V3 has 8MB flash but the default partition table uses 4MB. Harmless warning; updating partition table is a future task.

## Build

```bash
# Source ESP-IDF environment (required each shell session)
export IDF_PATH=~/src/esp-idf
IDF_VENV=$(ls -d "$HOME/.espressif/python_env"/idf*.4_py*_env 2>/dev/null | sort -V | tail -1 || true)
if [[ -n "${IDF_VENV:-}" ]]; then
  export PATH="$IDF_VENV/bin:$PATH"
fi
source $IDF_PATH/export.sh

# First time: set target chip
cd /path/to/bramble
idf.py set-target esp32s3

# Build (default profile)
idf.py build
```

### Build using board defaults profiles

```bash
# Heltec WiFi LoRa 32 V3 (default)
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" build

# Heltec WiFi LoRa 32 V4 (in-progress support)
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build

# LILYGO T-Deck Plus
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build
```

Build output: `build/bramble.bin` (~220KB)

## Flash

```bash
# Flash via USB (auto-detects port on most systems)
idf.py -p /dev/ttyUSB0 flash

# Or specify port explicitly
idf.py -p PORT flash
```

The Heltec V3 uses a **CP210x USB-to-UART bridge** (Silicon Labs). It appears as `/dev/ttyUSB0` on Linux.

## Monitor Serial Output

```bash
# Using idf.py (requires TTY)
idf.py -p /dev/ttyUSB0 monitor

# Or raw serial (works over SSH without TTY)
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
time.sleep(2)
print(s.read(4096).decode(errors='replace'))
s.close()
"
```

## Supported Hardware

| Board | Chip | LoRa | Status |
|-------|------|------|--------|
| **Heltec WiFi LoRa 32 V3** | ESP32-S3 (QFN56) | SX1262 | ✅ Primary target |
| **Heltec WiFi LoRa 32 V4** | ESP32-S3 | SX1262 (+ optional L76K GNSS) | 🔧 Bring-up in progress |
| LILYGO T-Beam Supreme | ESP32-S3 | SX1262 | 🔧 Needs pin config |
| LILYGO T-Deck Plus | ESP32-S3 | SX1262 | 🔧 Different display driver |

## Build Environment Reference

Verified working configuration:
- **ESP-IDF:** v5.4
- **Python:** 3.12
- **Target:** esp32s3
- **Flash:** GD 8MB (SPI DIO, 80MHz)
- **Binary size:** ~220KB (12% of 1.75MB app partition)
- **RAM:** ~140KB used, ~334KB available

## Running Tests (Host)

Unit tests run natively on the host (no hardware needed):

```bash
cd test/build
cmake ..
make -j$(nproc)
# Run all test binaries
for t in test_*; do ./$t; done
```

200 tests across 36 test suites.

## Running Webapp Tests

Run the webapp unit test suite locally:

```bash
cd webapp
npm ci
npm test
```

The webapp CI job uses the same command (`npm test`) in `webapp/` via `.github/workflows/webapp-tests.yml`.
