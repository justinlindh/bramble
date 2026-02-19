#!/usr/bin/env bash
# Build and flash Bramble locally (for T-Deck Plus connected to this machine)
# Usage: bash scripts/flash-local.sh [BOARD] [ACTION] [PORT]
#   BOARD: heltec-v3 (default), tdeck-plus
#   ACTION: flash (default), monitor
#   PORT: /dev/ttyACM0 (default for tdeck-plus), /dev/ttyUSB0 (default for heltec-v3)
#
# Examples:
#   bash scripts/flash-local.sh tdeck-plus
#   bash scripts/flash-local.sh tdeck-plus monitor
#   bash scripts/flash-local.sh tdeck-plus flash /dev/ttyACM1

set -euo pipefail

BOARD="${1:-heltec-v3}"
ACTION="${2:-flash}"
PORT="${3:-}"

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

# Check if user is in dialout group
IN_DIALOUT=0
if groups | grep -q '\bdialout\b'; then
    IN_DIALOUT=1
fi

# Wrapper function for serial access
run_serial_cmd() {
    if [ $IN_DIALOUT -eq 1 ]; then
        "$@"
    else
        echo "==> Not in dialout group, using sg wrapper..."
        sg dialout -c "$*"
    fi
}

# Monitor-only mode
if [ "$ACTION" = "monitor" ]; then
    echo "==> Monitoring $PORT..."
    run_serial_cmd idf.py -p "$PORT" monitor
    exit 0
fi

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
run_serial_cmd idf.py -p "$PORT" flash monitor
