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
# other nodes are plaintext.
#
# This script decides which is which by READING SPI_BOOT_CRYPT_CNT off each
# chip immediately before flashing it. The eFuse is the only authority: ports
# renumber, boards get swapped, and bench notes go stale in both directions.
# Trusting a written-down answer has already plaintext-bricked an encrypted
# board once. If the eFuse cannot be read, the node is SKIPPED rather than
# guessed at.
#
# BRAMBLE_ENCRYPTED_ADDRS is optional and is only a cross-check: if it
# disagrees with the silicon, the node is skipped and the mismatch reported,
# because one of the two is wrong and neither is safe to act on.
set -euo pipefail
cd "$(dirname "$0")/.."

ESPTOOL="${ESPTOOL:-esptool}"
ESPEFUSE="${ESPEFUSE:-espefuse}"
for tool in "$ESPTOOL" "$ESPEFUSE"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool not found on PATH." >&2
    echo "Install esptool (e.g. 'pipx install esptool') or set \$ESPTOOL/\$ESPEFUSE." >&2
    exit 1
  fi
done

# scripts/bramble-rpc needs pyserial. The system python3 usually lacks it, and
# the resulting failure looks exactly like "no node detected", so check up front.
ESPTOOL_PYTHON="${ESPTOOL_PYTHON:-}"
if [[ -z "$ESPTOOL_PYTHON" ]]; then
  for cand in "$(command -v python3 || true)" \
              "$HOME/.local/share/pipx/venvs/esptool/bin/python"; do
    [[ -x "$cand" ]] || continue
    if "$cand" -c "import serial" >/dev/null 2>&1; then ESPTOOL_PYTHON="$cand"; break; fi
  done
fi
if [[ -z "$ESPTOOL_PYTHON" ]] || ! "$ESPTOOL_PYTHON" -c "import serial" >/dev/null 2>&1; then
  echo "error: no python with pyserial found (scripts/bramble-rpc needs it)." >&2
  echo "Set \$ESPTOOL_PYTHON, e.g. ~/.local/share/pipx/venvs/esptool/bin/python" >&2
  exit 1
fi
PY="$ESPTOOL_PYTHON"
ENCRYPTED_ADDRS="${BRAMBLE_ENCRYPTED_ADDRS:-}"   # optional cross-check only

# Echo "encrypted", "plaintext", or "" (unreadable) for the chip on $1.
# Flash encryption is active when an ODD number of CRYPT_CNT bits is set
# (0b001 and 0b111 enable it; 0b011 does not).
read_crypt_state() {
  local port="$1" bits ones
  bits=$("$ESPEFUSE" --port "$port" summary 2>/dev/null \
         | grep -E "SPI_BOOT_CRYPT_CNT" \
         | grep -oE "0b[01]+" | tail -1 || true)
  [[ -n "$bits" ]] || return 0
  ones=${bits//[^1]/}
  if (( ${#ones} % 2 == 1 )); then echo "encrypted"; else echo "plaintext"; fi
}

if [[ "${1:-}" == "build" ]]; then
  bash scripts/flash.sh local heltec-v4 build
  bash scripts/flash.sh local heltec-v3 build
fi

failed=0
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

  crypt=$(read_crypt_state "$port")
  if [[ -z "$crypt" ]]; then
    echo "$port: $addr ($hw) -> SKIPPED, could not read SPI_BOOT_CRYPT_CNT" >&2
    echo "  refusing to guess: a plaintext flash of an encrypted node bricks it." >&2
    failed=1
    continue
  fi
  if [[ -n "$ENCRYPTED_ADDRS" ]]; then
    listed="plaintext"
    [[ " $ENCRYPTED_ADDRS " == *" $addr "* ]] && listed="encrypted"
    if [[ "$listed" != "$crypt" ]]; then
      echo "$port: $addr ($hw) -> SKIPPED, eFuse says $crypt but BRAMBLE_ENCRYPTED_ADDRS implies $listed" >&2
      echo "  the silicon is authoritative; correct the env var before reflashing." >&2
      failed=1
      continue
    fi
  fi

  build_dir="build-heltec-v4"
  [[ "$hw" == "heltec_v3" ]] && build_dir="build-heltec-v3"
  [[ "$hw" == "tdeck_plus" ]] && build_dir="build-tdeck-plus"
  if [[ "$crypt" == "encrypted" ]]; then
    echo "$port: $addr ($hw) -> ENCRYPTED app-only flash from $build_dir (eFuse-detected)"
    "$ESPTOOL" --chip esp32s3 --port "$port" -b 460800 \
      --before default_reset --after hard_reset \
      write_flash --encrypt 0x10000 "$build_dir/bramble.bin"
  else
    echo "$port: $addr ($hw) -> plaintext full flash from $build_dir (eFuse-detected)"
    (cd "$build_dir" && "$ESPTOOL" --chip esp32s3 --port "$port" -b 460800 \
      --before default_reset --after hard_reset write_flash @flash_args)
  fi
done
if (( failed )); then
  echo "One or more nodes were skipped without being flashed (see above)." >&2
  exit 1
fi
echo "Done. Verify identities with: scripts/bramble-rpc PORT bramble.getStatus"
