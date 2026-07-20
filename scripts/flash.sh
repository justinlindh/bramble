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
  - Extra args starting with -D are idf.py GLOBAL options and reach every
    idf.py invocation, including the build (example: -DPROJECT_VER=1.5.12
    to stamp a build with a release version). Other extra args are passed
    to `idf.py flash` only (example: --erase-nvs)
  - PROJECT_VER is pinned to 0.0.0-local unless -DPROJECT_VER=... is given,
    so a stale CMake cache can never stamp an old version silently
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

# --enable-antirollback is consumed by the anti-rollback guard, never passed
# to idf.py. Flashing a build with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is
# refused without it (plus a typed confirmation restating the irreversible
# eFuse burn); see scripts/antirollback-guard.sh and
# docs/design/ota-antirollback.md.
ANTIROLLBACK_ARGS=()
FILTERED_ARGS=()
for arg in ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}; do
  if [[ "$arg" == "--enable-antirollback" ]]; then
    ANTIROLLBACK_ARGS=(--enable-antirollback)
  else
    FILTERED_ARGS+=("$arg")
  fi
done
EXTRA_ARGS=(${FILTERED_ARGS[@]+"${FILTERED_ARGS[@]}"})

# Split the remaining extra args: -D... are idf.py GLOBAL options (cmake cache
# defines) and must precede the command on EVERY invocation, build included;
# everything else is a flash-command option and stays after `flash` as before.
# Historically -D args were silently dropped by the build action, so a
# -DPROJECT_VER=... never reached cmake and a stale cached version shipped
# onto devices unnoticed.
GLOBAL_ARGS=()
FLASH_CMD_ARGS=()
for arg in ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}; do
  if [[ "$arg" == -D* ]]; then
    GLOBAL_ARGS+=("$arg")
  else
    FLASH_CMD_ARGS+=("$arg")
  fi
done

# Pin PROJECT_VER on every configure unless the caller sets one. The CMake
# cache keeps the last -DPROJECT_VER forever, so without this a build dir
# once configured with a version keeps stamping it into every later build
# (bench builds shipped a months-old test version this way). Always passing
# an explicit value makes the stamp deterministic: callers get exactly what
# they asked for, and plain local builds always get 0.0.0-local.
have_project_ver=0
for arg in ${GLOBAL_ARGS[@]+"${GLOBAL_ARGS[@]}"}; do
  [[ "$arg" == -DPROJECT_VER=* ]] && have_project_ver=1
done
if [[ "$have_project_ver" -eq 0 ]]; then
  GLOBAL_ARGS+=("-DPROJECT_VER=0.0.0-local")
fi

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
  # Serial access: do not assume a group name (Debian uses dialout, Arch uses
  # uucp). Test actual write access to the port; only if that fails, wrap in
  # sg with the group that OWNS the device node.
  run_serial_cmd() {
    if [[ -z "$PORT" || -w "$PORT" ]]; then
      "$@"
    elif [[ -e "$PORT" ]]; then
      local grp
      grp=$(stat -c %G "$PORT")
      echo "==> No write access to $PORT, using sg $grp wrapper..."
      sg "$grp" -c "$*"
    else
      "$@"
    fi
  }

  case "$ACTION" in
    build)
      echo "==> Building locally..."
      [[ ! -d "$BOARD_BUILD_DIR" ]] && rm -f "$BOARD_SDKCONFIG"
      idf.py "${IDF_BOARD_ARGS[@]}" "${GLOBAL_ARGS[@]}" build
      # Loud notice (never blocks a build) if this is an anti-rollback build.
      bash scripts/antirollback-guard.sh --sdkconfig "$BOARD_SDKCONFIG" --action build
      ;;
    monitor)
      echo "==> Monitoring $PORT..."
      run_serial_cmd idf.py "${IDF_BOARD_ARGS[@]}" -p "$PORT" monitor
      ;;
    flash)
      echo "==> Building locally..."
      [[ ! -d "$BOARD_BUILD_DIR" ]] && rm -f "$BOARD_SDKCONFIG"
      idf.py "${IDF_BOARD_ARGS[@]}" "${GLOBAL_ARGS[@]}" build
      # Anti-rollback consent gate: refuses to flash an anti-rollback build
      # unless --enable-antirollback was passed AND the operator types the
      # epoch-specific confirmation. A non-anti-rollback build passes silently.
      bash scripts/antirollback-guard.sh --sdkconfig "$BOARD_SDKCONFIG" --action flash \
        --port "$PORT" ${ANTIROLLBACK_ARGS[@]+"${ANTIROLLBACK_ARGS[@]}"}
      echo "==> Flashing to $PORT (serial)..."
      run_serial_cmd idf.py "${IDF_BOARD_ARGS[@]}" "${GLOBAL_ARGS[@]}" -p "$PORT" flash \
        ${FLASH_CMD_ARGS[@]+"${FLASH_CMD_ARGS[@]}"}
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
