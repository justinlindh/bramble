#!/usr/bin/env bash
# Build and flash Bramble locally (for T-Deck Plus connected to this machine)
# Usage: bash scripts/flash-local.sh [BOARD] [PORT]
#   BOARD: heltec-v3 (default), tdeck-plus
#   PORT: /dev/ttyACM0 (default for tdeck-plus), /dev/ttyUSB0 (default for heltec-v3)

set -euo pipefail

BOARD="${1:-heltec-v3}"
PORT="${2:-}"

# Set default port based on board if not specified
if [ -z "$PORT" ]; then
    if [ "$BOARD" = "tdeck-plus" ]; then
        PORT="/dev/ttyACM0"
    else
        PORT="/dev/ttyUSB0"
    fi
fi

# Source ESP-IDF environment
if [ ! -f "$HOME/src/esp-idf/export.sh" ]; then
    echo "ERROR: ESP-IDF not found at ~/src/esp-idf"
    exit 1
fi

echo "==> Sourcing ESP-IDF environment..."
source "$HOME/src/esp-idf/export.sh"

cd "$(dirname "$0")/.."

# Set sdkconfig defaults based on board
if [ "$BOARD" = "tdeck-plus" ]; then
    export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus"
    echo "==> Building for T-Deck Plus (ESP32-S3)"
else
    export SDKCONFIG_DEFAULTS="sdkconfig.defaults"
    echo "==> Building for Heltec V3 (ESP32-S3)"
fi

# Clean build config if switching boards
if [ -f "sdkconfig" ]; then
    echo "==> Cleaning sdkconfig (board switch detected)"
    rm -f sdkconfig
fi

# Build
echo "==> Building..."
idf.py build

# Flash
echo "==> Flashing to $PORT..."
idf.py -p "$PORT" flash monitor
