#!/usr/bin/env bash
# Flash every connected bench node with the current builds, applying the
# per-node encryption rule automatically.
#
#   bash scripts/flash-fleet.sh          # flash all detected nodes
#   bash scripts/flash-fleet.sh build    # rebuild both board images first
#
#   BRAMBLE_ENCRYPTED_ADDRS="11223344 55667788" bash scripts/flash-fleet.sh
#
# HARD RULE: a node whose flash-encryption eFuse is burned MUST be flashed
# app-only with --encrypt or it bricks and can lose its NVS identity. All
# other nodes are plaintext. List the ADDRESSES (space-separated) of your
# encrypted-eFuse nodes in the BRAMBLE_ENCRYPTED_ADDRS env var; identification
# is by node ADDRESS (ports renumber whenever a device is replugged), read
# over serial before flashing.
set -euo pipefail
cd "$(dirname "$0")/.."

ESPTOOL_PYTHON="${ESPTOOL_PYTHON:-python3}"
ESPTOOL="${ESPTOOL:-esptool}"
if ! command -v "$ESPTOOL" >/dev/null 2>&1; then
  echo "error: esptool not found (looked for '$ESPTOOL' on PATH)." >&2
  echo "Install esptool (e.g. 'pipx install esptool') or set \$ESPTOOL to its path." >&2
  exit 1
fi
PY="$ESPTOOL_PYTHON"
ENCRYPTED_ADDRS="${BRAMBLE_ENCRYPTED_ADDRS:-}"   # space-separated list of encrypted-eFuse node addresses

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
