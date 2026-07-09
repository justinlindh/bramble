#!/usr/bin/env bash
# Flash every connected bench node with the current builds, applying the
# per-node encryption rule automatically.
#
#   bash scripts/flash-fleet.sh          # flash all detected nodes
#   bash scripts/flash-fleet.sh build    # rebuild both board images first
#
# HARD RULE: node CAFEBABE (the bench Heltec V3) has flash encryption fused
# on. It MUST be flashed app-only with --encrypt or it bricks and can lose
# its NVS identity. All other bench nodes are plaintext. Identification is
# by node ADDRESS (ports renumber whenever a device is replugged), read over
# serial before flashing.
set -euo pipefail
cd "$(dirname "$0")/.."

PY=~/.local/share/pipx/venvs/esptool/bin/python
ESPTOOL=~/.local/share/pipx/venvs/esptool/bin/esptool
ENCRYPTED_ADDRS="CAFEBABE"   # space-separated list of encrypted-eFuse nodes

if [[ "${1:-}" == "build" ]]; then
  bash scripts/flash.sh local heltec-v4 build
  bash scripts/flash.sh local heltec-v3 build
fi

for port in /dev/ttyACM* /dev/ttyUSB*; do
  [[ -e "$port" ]] || continue
  extra=""
  [[ "$port" == /dev/ttyUSB* ]] && extra="--cp2102"
  status=$("$PY" scripts/bramble-rpc "$port" bramble.getStatus $extra 2>/dev/null || true)
  addr=$(echo "$status" | python3 -c "import json,sys
try: print(json.load(sys.stdin).get('address',''))
except Exception: print('')" 2>/dev/null || true)
  hw=$(echo "$status" | python3 -c "import json,sys
try: print(json.load(sys.stdin).get('hardware',''))
except Exception: print('')" 2>/dev/null || true)
  if [[ -z "$addr" ]]; then
    echo "$port: no bramble node detected, skipping"
    continue
  fi
  build_dir="build-heltec-v4"
  [[ "$hw" == "heltec_v3" ]] && build_dir="build-heltec-v3"
  [[ "$hw" == "tdeck_plus" ]] && build_dir="build-tdeck-plus"
  if [[ " $ENCRYPTED_ADDRS " == *" $addr "* ]]; then
    echo "$port: $addr ($hw) -> ENCRYPTED app-only flash from $build_dir"
    "$ESPTOOL" --chip esp32s3 --port "$port" -b 460800 \
      --before default_reset --after hard_reset \
      write_flash --encrypt 0x10000 "$build_dir/bramble.bin"
  else
    echo "$port: $addr ($hw) -> plaintext full flash from $build_dir"
    (cd "$build_dir" && "$ESPTOOL" --chip esp32s3 --port "$port" -b 460800 \
      --before default_reset --after hard_reset write_flash @flash_args)
  fi
done
echo "Done. Verify identities with: scripts/bramble-rpc PORT bramble.getStatus"
