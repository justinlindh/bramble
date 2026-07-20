#!/usr/bin/env bash
# antirollback-guard.sh: the deliberate-consent gate for eFuse anti-rollback
# builds (issue #79, docs/design/ota-antirollback.md).
#
# Booting an image built with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK on a device
# whose bootloader enforces it IRREVERSIBLY burns the eFuse secure-version
# floor up to the image's secure_version. After that, the device permanently
# refuses to boot any image with a lower secure_version, on every transport,
# including a USB reflash. There is no undo.
#
# This script is called by scripts/flash.sh (and, through it, by
# scripts/flash-all.py) after the build and before any flash. Behavior:
#
#   - Build without CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK: silent pass (exit 0).
#     The default dev flow is completely unaffected.
#   - Anti-rollback build, action "build": loud banner, pass (building burns
#     nothing; only booting on an enforcing bootloader does).
#   - Anti-rollback build, action "flash": REFUSED (exit 2) unless the
#     operator passed --enable-antirollback AND types the exact confirmation
#     phrase "BURN EPOCH <N>" where N is the image's secure_version. The
#     epoch-specific phrase makes every ratchet to a new epoch its own
#     explicit, typed decision. The current device floor is shown best-effort
#     via espefuse.py when a port is available, and stated as UNKNOWN
#     otherwise.
#
# Exit codes: 0 = proceed, 2 = refused or usage error.
#
# Usage:
#   antirollback-guard.sh --sdkconfig FILE --action build|flash \
#       [--port PORT] [--enable-antirollback]

set -euo pipefail

SDKCONFIG=""
ACTION=""
PORT=""
ENABLE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sdkconfig)
      SDKCONFIG="${2:?--sdkconfig needs a value}"
      shift 2
      ;;
    --action)
      ACTION="${2:?--action needs a value}"
      shift 2
      ;;
    --port)
      PORT="${2:-}"
      shift 2
      ;;
    --enable-antirollback)
      ENABLE=1
      shift
      ;;
    *)
      echo "antirollback-guard: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$SDKCONFIG" || -z "$ACTION" ]]; then
  echo "antirollback-guard: usage: --sdkconfig FILE --action build|flash [--port PORT] [--enable-antirollback]" >&2
  exit 2
fi
if [[ "$ACTION" != "build" && "$ACTION" != "flash" ]]; then
  echo "antirollback-guard: --action must be build or flash, got '$ACTION'" >&2
  exit 2
fi
if [[ ! -f "$SDKCONFIG" ]]; then
  # Fail closed: without the effective config we cannot prove the build is
  # not an anti-rollback build.
  echo "antirollback-guard: sdkconfig not found: $SDKCONFIG (cannot verify build; refusing)" >&2
  exit 2
fi

# Not an anti-rollback build: silent pass, the default flow is untouched.
if ! grep -qE '^CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y' "$SDKCONFIG"; then
  exit 0
fi

EPOCH="$(grep -E '^CONFIG_BOOTLOADER_APP_SECURE_VERSION=' "$SDKCONFIG" | head -1 | cut -d= -f2 || true)"
EPOCH="${EPOCH:-0}"

EMULATED=0
if grep -qE '^CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE=y' "$SDKCONFIG"; then
  EMULATED=1
fi

banner() {
  echo "############################################################################"
  echo "##  ANTI-ROLLBACK BUILD (eFuse secure version)                            ##"
  echo "############################################################################"
  echo "##  This image carries secure_version (epoch) = $EPOCH"
  if (( EMULATED )); then
    echo "##  MODE: EMULATED (CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE=y)."
    echo "##  The secure-version field lives in a flash partition, NOT real eFuses."
    echo "##  Reversible by erasing flash. No permanent burn in this mode."
  else
    echo "##  MODE: REAL eFUSES. Booting this image on a device whose bootloader"
    echo "##  enforces anti-rollback PERMANENTLY and IRREVERSIBLY burns the eFuse"
    echo "##  floor up to epoch $EPOCH. The device will then refuse to boot ANY"
    echo "##  image with a lower secure_version, forever, on every transport,"
    echo "##  including USB reflash. There is no undo."
  fi
  echo "##  See docs/design/ota-antirollback.md before proceeding."
  echo "############################################################################"
}

banner

if [[ "$ACTION" == "build" ]]; then
  # Building is safe; only booting on an enforcing bootloader burns.
  exit 0
fi

if (( ! ENABLE )); then
  echo "" >&2
  echo "antirollback-guard: REFUSING to flash an anti-rollback build without" >&2
  echo "explicit consent. The default flash flow never carries a secure_version" >&2
  echo "burn onto a device silently." >&2
  echo "" >&2
  echo "If you really intend this (sacrificial/bench board, procedure in" >&2
  echo "docs/design/ota-antirollback.md section 7), re-run with:" >&2
  echo "    --enable-antirollback" >&2
  exit 2
fi

# Best-effort read of the device's current burned floor for the
# old-floor -> new-floor display. espefuse resets the target, which is
# acceptable immediately before a flash.
DEVICE_FLOOR_INFO="UNKNOWN (no port given or espefuse.py unavailable; assume the first boot ratchets the floor up to epoch $EPOCH)"
if [[ -n "$PORT" ]] && command -v espefuse.py >/dev/null 2>&1; then
  SUMMARY="$(timeout 30 espefuse.py --port "$PORT" summary 2>/dev/null || true)"
  LINE="$(printf '%s\n' "$SUMMARY" | grep -i 'SECURE_VERSION' | head -2 || true)"
  if [[ -n "$LINE" ]]; then
    DEVICE_FLOOR_INFO="$LINE"
  else
    DEVICE_FLOOR_INFO="UNKNOWN (espefuse read failed on $PORT; assume the first boot ratchets the floor up to epoch $EPOCH)"
  fi
fi

echo ""
echo "Current device secure-version eFuse state:"
echo "    $DEVICE_FLOOR_INFO"
echo "This image will move the floor to: epoch $EPOCH"
echo ""
if (( EMULATED )); then
  echo "Emulated mode: this ratchet is reversible by erasing flash."
else
  echo "This is IRREVERSIBLE. If you later need to run firmware below epoch"
  echo "$EPOCH on this device, you will not be able to. There is no undo."
fi
echo ""
echo "Type exactly:  BURN EPOCH $EPOCH   to proceed (anything else aborts)."
printf "> "

CONFIRM=""
if ! IFS= read -r CONFIRM; then
  echo "" >&2
  echo "antirollback-guard: no confirmation received (EOF); refusing." >&2
  exit 2
fi

if [[ "$CONFIRM" != "BURN EPOCH $EPOCH" ]]; then
  echo "antirollback-guard: confirmation mismatch; refusing." >&2
  exit 2
fi

echo "antirollback-guard: confirmed; proceeding with anti-rollback flash (epoch $EPOCH)."
exit 0
