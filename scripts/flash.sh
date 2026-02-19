#!/usr/bin/env bash
# Build and flash Bramble to Heltec V3 via GPU box (192.168.1.199)
# Usage: bash scripts/flash.sh [PORT]
# Assumes code is already committed and pushed to Gitea.
#
# NOTE: For T-Deck Plus, use scripts/flash-local.sh instead (USB-connected to local machine).
set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
GPU_BOX="192.168.1.199"
REMOTE_DIR="~/src/bramble"

echo "==> Pulling latest on GPU box..."
ssh "$GPU_BOX" "cd $REMOTE_DIR && git pull"

echo "==> Building..."
ssh "$GPU_BOX" "bash -c 'export IDF_PATH=~/src/esp-idf && source \$IDF_PATH/export.sh 2>/dev/null && cd $REMOTE_DIR && idf.py build 2>&1 | tail -5'"

echo "==> Flashing to $PORT..."
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

echo "==> Done!"
