#!/usr/bin/env bash
# Unified Bramble build/flash helper (serial flashing only; no OTA)
#
# Preferred usage:
#   bash scripts/flash.sh [local] [heltec-v3|heltec-v4|tdeck-plus|bramble-pager] [flash|monitor|build] [PORT] [extra idf.py args...]

set -euo pipefail

LOCAL_DIR="$(cd "$(dirname "$0")/.." && pwd)"

MODE="local"
BOARD="heltec-v3"
ACTION="flash"
PORT=""
EXTRA_ARGS=()

is_mode() { [[ "$1" == "local" ]]; }
is_board() { [[ "$1" == "heltec-v3" || "$1" == "heltec-v4" || "$1" == "tdeck-plus" || "$1" == "bramble-pager" ]]; }
is_action() { [[ "$1" == "flash" || "$1" == "monitor" || "$1" == "build" ]]; }
is_port() { [[ "$1" == /dev/* ]]; }

print_usage() {
  cat <<'EOF'
Usage:
  bash scripts/flash.sh [local] [heltec-v3|heltec-v4|tdeck-plus|bramble-pager] [flash|monitor|build] [PORT] [extra idf.py args...]

Notes:
  - MODE defaults to: local
  - BOARD defaults to: heltec-v3
  - ACTION defaults to: flash
  - Default PORT: /dev/ttyACM0 for tdeck-plus and bramble-pager (native USB), /dev/ttyUSB0 otherwise
  - Extra args are passed to `idf.py flash` (example: --erase-nvs)
EOF
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
  print_usage
  exit 0
fi

if [[ $# -gt 0 ]] && is_mode "$1"; then
  MODE="$1"
  shift
fi

if [[ $# -gt 0 ]] && is_board "$1"; then
  BOARD="$1"
  shift
fi

if [[ $# -gt 0 ]] && is_action "$1"; then
  ACTION="$1"
  shift
fi

if [[ $# -gt 0 && "$1" == "--monitor" ]]; then
  ACTION="monitor"
  shift
fi

if [[ $# -gt 0 ]] && is_port "$1"; then
  PORT="$1"
  shift
fi

if [[ $# -gt 0 ]]; then
  EXTRA_ARGS=("$@")
fi

# Refuse unrecognized positional tokens instead of absorbing them.
#
# Every is_* check above is an exact match, so a near-miss like "heltec_v4"
# (underscore) or "tdeck_plus" matches NOTHING: it is not a board, not an
# action, not a port. Before this guard it silently fell through to EXTRA_ARGS
# and left BOTH defaults standing, which are the two most dangerous values in
# the script: BOARD=heltec-v3 (the flash-encrypted bench node, which a
# plaintext flash bricks and strips of its NVS identity) and ACTION=flash.
# "flash.sh local heltec_v4 build" therefore meant "plaintext-flash the
# encrypted V3", which is never what anyone typed it to mean. Real extra idf.py
# args are flags (--erase-nvs), so anything not starting with '-' here is a typo.
for arg in ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}; do
  if [[ "$arg" != -* ]]; then
    echo "flash.sh: unrecognized argument: '$arg'" >&2
    echo "Boards: heltec-v3 heltec-v4 tdeck-plus bramble-pager (HYPHENS, not underscores)" >&2
    echo "Actions: flash monitor build" >&2
    echo "Extra idf.py args must start with '-'." >&2
    echo >&2
    print_usage >&2
    exit 2
  fi
done

if [[ -z "$PORT" ]]; then
  if [[ "$BOARD" == "tdeck-plus" || "$BOARD" == "bramble-pager" ]]; then
    PORT="/dev/ttyACM0"
  else
    PORT="/dev/ttyUSB0"
  fi
fi

set_board_vars() {
  case "$BOARD" in
    tdeck-plus)
      BOARD_NAME="T-Deck Plus"
      BOARD_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus"
      BOARD_BUILD_DIR="build-tdeck-plus"
      ;;
    heltec-v3)
      BOARD_NAME="Heltec V3"
      BOARD_DEFAULTS="sdkconfig.defaults"
      BOARD_BUILD_DIR="build-heltec-v3"
      ;;
    heltec-v4)
      BOARD_NAME="Heltec V4"
      BOARD_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4"
      BOARD_BUILD_DIR="build-heltec-v4"
      ;;
    bramble-pager)
      BOARD_NAME="Bramble Pager v1"
      BOARD_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bramble_pager"
      BOARD_BUILD_DIR="build-bramble-pager"
      ;;
    *)
      echo "Unsupported board: $BOARD"
      exit 1
      ;;
  esac

  BOARD_SDKCONFIG="sdkconfig.${BOARD}"

  IDF_BOARD_ARGS=(
    -B "$BOARD_BUILD_DIR"
    -D "SDKCONFIG=$BOARD_SDKCONFIG"
    -D "SDKCONFIG_DEFAULTS=$BOARD_DEFAULTS"
  )

  echo "==> Target board: $BOARD_NAME"
  echo "==> Build dir: $BOARD_BUILD_DIR"
  echo "==> SDKCONFIG: $BOARD_SDKCONFIG"
  echo "==> SDKCONFIG_DEFAULTS: $BOARD_DEFAULTS"
}

prepare_local_env() {
  local idf_root="${IDF_PATH:-}"

  if [[ -z "$idf_root" ]]; then
    for candidate in "$HOME/src/esp-idf" "$HOME/esp-idf" "/opt/esp/idf" "/opt/esp-idf"; do
      if [[ -f "$candidate/export.sh" ]]; then
        idf_root="$candidate"
        break
      fi
    done
  fi

  if [[ -z "$idf_root" || ! -f "$idf_root/export.sh" ]]; then
    echo "ERROR: ESP-IDF not found (tried IDF_PATH, ~/src/esp-idf, ~/esp-idf, /opt/esp/idf, /opt/esp-idf)"
    exit 1
  fi

  IDF_VENV=$(ls -d "$HOME/.espressif/python_env"/idf*.4_py*_env 2>/dev/null | sort -V | tail -1 || true)
  if [[ -n "${IDF_VENV:-}" && -x "$IDF_VENV/bin/python3" ]]; then
    export PATH="$IDF_VENV/bin:$PATH"
    echo "==> Using Python from $IDF_VENV ($($IDF_VENV/bin/python3 --version))"
  fi

  export IDF_PATH="$idf_root"

  if command -v idf.py >/dev/null 2>&1; then
    echo "==> Reusing existing ESP-IDF environment (idf.py already on PATH)"
  else
    # shellcheck source=/dev/null
    source "$IDF_PATH/export.sh"
  fi

  cd "$LOCAL_DIR"
  bash scripts/ensure-ota-signing-key.sh
  set_board_vars
}

run_local() {
  local in_dialout=0
  if groups | grep -q '\bdialout\b'; then
    in_dialout=1
  fi

  run_serial_cmd() {
    if [[ $in_dialout -eq 1 ]]; then
      "$@"
    else
      echo "==> Not in dialout group, using sg wrapper..."
      sg dialout -c "$*"
    fi
  }

  case "$ACTION" in
    build)
      echo "==> Building locally..."
      [[ ! -d "$BOARD_BUILD_DIR" ]] && rm -f "$BOARD_SDKCONFIG"
      idf.py "${IDF_BOARD_ARGS[@]}" build
      ;;
    monitor)
      echo "==> Monitoring $PORT..."
      run_serial_cmd idf.py "${IDF_BOARD_ARGS[@]}" -p "$PORT" monitor
      ;;
    flash)
      echo "==> Building locally..."
      [[ ! -d "$BOARD_BUILD_DIR" ]] && rm -f "$BOARD_SDKCONFIG"
      idf.py "${IDF_BOARD_ARGS[@]}" build
      echo "==> Flashing to $PORT (serial)..."
      run_serial_cmd idf.py "${IDF_BOARD_ARGS[@]}" -p "$PORT" flash "${EXTRA_ARGS[@]}"
      ;;
    *)
      print_usage
      exit 1
      ;;
  esac
}

if [[ "$MODE" == "local" ]]; then
  prepare_local_env
  run_local
else
  print_usage
  exit 1
fi

echo "==> Done!"
