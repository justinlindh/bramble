#!/usr/bin/env bash
# Unified Bramble build/flash helper (serial flashing only; no OTA)
#
# Preferred usage:
#   bash scripts/flash.sh [local|gpu] [heltec-v3|tdeck-plus] [flash|monitor|build] [PORT] [extra idf.py args...]

set -euo pipefail

GPU_BOX="192.0.2.199"
REMOTE_DIR="~/src/bramble"
LOCAL_DIR="$(cd "$(dirname "$0")/.." && pwd)"

MODE="local"
BOARD="heltec-v3"
ACTION="flash"
PORT=""
EXTRA_ARGS=()

is_mode() { [[ "$1" == "local" || "$1" == "gpu" ]]; }
is_board() { [[ "$1" == "heltec-v3" || "$1" == "heltec-v4" || "$1" == "tdeck-plus" ]]; }
is_action() { [[ "$1" == "flash" || "$1" == "monitor" || "$1" == "build" ]]; }
is_port() { [[ "$1" == /dev/* ]]; }

print_usage() {
  cat <<'EOF'
Usage:
  bash scripts/flash.sh [local|gpu] [heltec-v3|tdeck-plus] [flash|monitor|build] [PORT] [extra idf.py args...]

Notes:
  - MODE defaults to: local
  - BOARD defaults to: heltec-v3
  - ACTION defaults to: flash
  - Default PORT: /dev/ttyACM0 for tdeck-plus, /dev/ttyUSB0 otherwise
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

# Legacy compatibility: allow "bash scripts/flash.sh gpu /dev/ttyUSB0"
if [[ "$MODE" == "gpu" && $# -gt 0 ]] && is_port "$1"; then
  PORT="$1"
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

if [[ -z "$PORT" ]]; then
  if [[ "$BOARD" == "tdeck-plus" ]]; then
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
  # shellcheck source=/dev/null
  source "$IDF_PATH/export.sh"
  cd "$LOCAL_DIR"
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

run_gpu() {
  if [[ "$BOARD" != "heltec-v3" ]]; then
    echo "ERROR: GPU mode currently supports heltec-v3 only (USB is on GPU box)."
    echo "Use local mode for tdeck-plus: bash scripts/flash.sh local tdeck-plus"
    exit 1
  fi

  local bundle_dir="$LOCAL_DIR/$BOARD_BUILD_DIR"
  local bundle_tmp="/tmp/bramble-flash-${BOARD}-$$"
  local bundle_tgz="/tmp/bramble-flash-${BOARD}-$$.tgz"

  # Build artifacts locally (source of truth), then flash GPU-connected device from those binaries.
  if [[ "$ACTION" == "build" || "$ACTION" == "flash" ]]; then
    echo "==> Building locally for GPU flash artifacts..."
    (cd "$LOCAL_DIR" && idf.py "${IDF_BOARD_ARGS[@]}" build)

    if [[ ! -f "$bundle_dir/flash_args" ]]; then
      echo "ERROR: Missing $bundle_dir/flash_args after local build"
      exit 1
    fi

    echo "==> Packaging local flash artifacts..."
    tar -C "$bundle_dir" -czf "$bundle_tgz" \
      flash_args \
      bramble.bin \
      ota_data_initial.bin \
      bootloader/bootloader.bin \
      partition_table/partition-table.bin
  fi

  if [[ "$ACTION" == "build" ]]; then
    rm -f "$bundle_tgz"
    echo "==> Build complete (local artifacts prepared for heltec-v3)."
    return
  fi

  if [[ "$ACTION" == "flash" ]]; then
    echo "==> Uploading flash artifacts to GPU box..."
    scp "$bundle_tgz" "$GPU_BOX:$bundle_tgz" >/dev/null

    echo "==> Flashing GPU-connected device from uploaded binaries (no remote source build)..."
    ssh "$GPU_BOX" "bash -lc '
      set -euo pipefail
      export IDF_PATH=~/src/esp-idf
      source \$IDF_PATH/export.sh >/dev/null 2>&1
      rm -rf "$bundle_tmp"
      mkdir -p "$bundle_tmp"
      tar -xzf "$bundle_tgz" -C "$bundle_tmp"
      cd "$bundle_tmp"
      python \$IDF_PATH/components/esptool_py/esptool/esptool.py --chip esp32s3 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash @flash_args ${EXTRA_ARGS[*]-}
      rm -rf "$bundle_tmp" "$bundle_tgz"
    '"

    rm -f "$bundle_tgz"
  elif [[ "$ACTION" == "monitor" ]]; then
    echo "==> Monitoring $PORT on GPU box..."
    ssh "$GPU_BOX" "bash -lc 'export IDF_PATH=~/src/esp-idf && source \$IDF_PATH/export.sh >/dev/null 2>&1 && cd $REMOTE_DIR && idf.py -p $PORT monitor'"
  fi
}

if [[ "$MODE" == "local" ]]; then
  prepare_local_env
  run_local
elif [[ "$MODE" == "gpu" ]]; then
  prepare_local_env
  run_gpu
else
  print_usage
  exit 1
fi

echo "==> Done!"
