#!/usr/bin/env bash
# Build and flash Bramble firmware
# Usage: bash scripts/flash.sh [local|gpu] [PORT]
#   local (default): build+flash on this machine
#   gpu:             build+flash via GPU box (192.168.1.199)
#
# NOTE: For T-Deck Plus, use scripts/flash-local.sh instead (USB-connected to local machine).
set -euo pipefail

MODE="${1:-local}"
PORT="${2:-/dev/ttyUSB0}"
GPU_BOX="192.168.1.199"
REMOTE_DIR="~/src/bramble"
LOCAL_DIR="$(cd "$(dirname "$0")/.." && pwd)"

flash_local() {
    echo "==> Building locally..."
    bash -c "source ~/src/esp-idf/export.sh 2>/dev/null && cd '$LOCAL_DIR' && idf.py build 2>&1 | tail -5"

    echo "==> Flashing to $PORT..."
    sg dialout -c "bash -c 'source ~/src/esp-idf/export.sh 2>/dev/null && cd \"$LOCAL_DIR\" && idf.py -p $PORT flash 2>&1 | tail -10'"

    echo "==> Reading boot log..."
    sg dialout -c "python3 -c \"
import serial, time
s = serial.Serial('$PORT', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
time.sleep(3)
print(s.read(8192).decode(errors='replace'))
s.close()
\""
}

flash_gpu() {
    echo "==> Pulling latest on GPU box..."
    ssh "$GPU_BOX" "cd $REMOTE_DIR && git pull"

    echo "==> Building on GPU box..."
    ssh "$GPU_BOX" "bash -c 'export IDF_PATH=~/src/esp-idf && source \$IDF_PATH/export.sh 2>/dev/null && cd $REMOTE_DIR && idf.py build 2>&1 | tail -5'"

    echo "==> Flashing to $PORT on GPU box..."
    ssh "$GPU_BOX" "bash -c 'export IDF_PATH=~/src/esp-idf && source \$IDF_PATH/export.sh 2>/dev/null && cd $REMOTE_DIR && idf.py -p $PORT flash 2>&1 | tail -10'"

    echo "==> Reading boot log..."
    ssh "$GPU_BOX" "python3 -c \"
import serial, time
s = serial.Serial('$PORT', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
time.sleep(3)
print(s.read(8192).decode(errors='replace'))
s.close()
\""
}

case "$MODE" in
    local) flash_local ;;
    gpu)   flash_gpu ;;
    *)     echo "Usage: $0 [local|gpu] [PORT]"; exit 1 ;;
esac

echo "==> Done!"
