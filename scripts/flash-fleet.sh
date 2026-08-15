#!/usr/bin/env bash
# Flash every connected bench node with the current builds, applying the
# per-node encryption rule automatically.
#
#   bash scripts/flash-fleet.sh          # flash all detected nodes
#   bash scripts/flash-fleet.sh build    # rebuild every board image first
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

# shellcheck source=scripts/lib/crypt-state.sh
source scripts/lib/crypt-state.sh

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

# Echo "encrypted", "plaintext", or "" (unreadable) for the chip on $1. The
# summary is parsed by crypt_state_from_summary (scripts/lib/crypt-state.sh),
# the same odd-parity rule flash.sh applies, so the two paths cannot diverge.
read_crypt_state() {
  local port="$1" summary
  # Swallow an espefuse failure to an empty summary (|| true) so the parser
  # echoes "" and the caller skips this node, rather than pipefail aborting the
  # whole fleet run mid-loop.
  summary=$("$ESPEFUSE" --port "$port" summary 2>/dev/null || true)
  printf '%s\n' "$summary" | crypt_state_from_summary
}

if [[ "${1:-}" == "build" ]]; then
  # Build every shipped board, not a subset: a detected node is flashed from
  # its own board image below, so any board omitted here would either be
  # flashed from a stale image or skipped outright.
  for board in heltec-v3 heltec-v4 tdeck-plus bramble-pager; do
    bash scripts/flash.sh local "$board" build
  done
fi

failed=0
for port in /dev/ttyACM* /dev/ttyUSB*; do
  [[ -e "$port" ]] || continue
  extra=""
  [[ "$port" == /dev/ttyUSB* ]] && extra="--cp2102"
  status=$("$PY" scripts/bramble-rpc "$port" bramble.getStatus $extra 2>/dev/null || true)
  # Parse address and hardware out of the one status response in a single
  # pass; a tab separates them so an empty field stays empty.
  fields=$(echo "$status" | python3 -c "import json,sys
try:
    d = json.load(sys.stdin)
    print(d.get('address',''), d.get('hardware',''), sep='\t')
except Exception:
    print('\t')" 2>/dev/null || true)
  addr=${fields%%$'\t'*}
  hw=${fields##*$'\t'}
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

  # Map the node's reported hardware (underscored short_name, e.g. heltec_v3)
  # to its build directory (hyphenated, e.g. build-heltec-v3), the same naming
  # flash.sh writes. Deriving it mechanically covers every board without a
  # per-board list here that silently falls behind the board matrix: the old
  # two-board list defaulted every other node (a real bramble-pager included)
  # to the heltec-v4 image. If the derived image is absent, SKIP rather than
  # fall back to another board's binary; flashing the wrong firmware is the
  # same class of harm as the mis-encrypted flash this script already refuses
  # to guess at.
  build_dir="build-${hw//_/-}"
  if [[ -z "$hw" || ! -e "$build_dir/bramble.bin" ]]; then
    echo "$port: $addr ($hw) -> SKIPPED, no image at $build_dir/bramble.bin" >&2
    echo "  build it first ('scripts/flash-fleet.sh build'); refusing to flash another board's image." >&2
    failed=1
    continue
  fi
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
