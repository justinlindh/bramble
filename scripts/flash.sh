#!/usr/bin/env bash
# Unified Bramble build/flash helper (serial flashing only; no OTA)
#
# Preferred usage:
#   bash scripts/flash.sh [local] [heltec-v3|heltec-v4|tdeck-plus|bramble-pager] [flash|encrypted-flash|monitor|build] [PORT] [extra idf.py args...]

set -euo pipefail

LOCAL_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# shellcheck source=scripts/lib/crypt-state.sh
source "$LOCAL_DIR/scripts/lib/crypt-state.sh"

BOARD="heltec-v3"
ACTION="flash"
PORT=""
EXTRA_ARGS=()

is_board() { [[ "$1" == "heltec-v3" || "$1" == "heltec-v4" || "$1" == "tdeck-plus" || "$1" == "bramble-pager" ]]; }
is_action() {
  [[ "$1" == "flash" || "$1" == "monitor" || "$1" == "build" || "$1" == "encrypted-flash" ]]
}
is_port() { [[ "$1" == /dev/* ]]; }

print_usage() {
  cat <<'EOF'
Usage:
  bash scripts/flash.sh [local] [heltec-v3|heltec-v4|tdeck-plus|bramble-pager] [flash|encrypted-flash|monitor|build] [PORT] [extra idf.py args...]

Notes:
  - The leading "local" token is optional and accepted for compatibility
    (serial-local is the only build path); it is skipped if present
  - BOARD defaults to: heltec-v3
  - ACTION defaults to: flash. Use encrypted-flash for a board whose
    flash-encryption eFuse is burned; both actions verify the eFuse first
    and refuse a mismatch, since either wrong choice bricks the board.
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

# Serial-local is the only build path (this script does no OTA/remote flashing).
# The optional leading "local" token is a compatibility no-op: callers and docs
# still pass it, so accept and skip it rather than mistaking it for a board name.
if [[ ${1:-} == "local" ]]; then
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
    echo "Actions: flash encrypted-flash monitor build" >&2
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

# Serial access: do not assume a group name (Debian uses dialout, Arch uses
# uucp). Test actual write access to the port; only if that fails, wrap in
# sg with the group that OWNS the device node. Top-level, not nested inside
# run_local: assert_encryption_matches_action needs it too, and relying on
# run_local having defined it first is a call-order dependency a refactor
# would quietly break.
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

# Refuse to write the wrong kind of image to a board.
#
# A node with flash encryption burned reads its flash through the decryption
# engine, so a plaintext image written to it decrypts to noise and the board
# does not boot: this has already bricked the encrypted V3 bench node once, and
# it takes an image reflash over a wire to recover. The reverse is just as
# broken: an encrypted image on a board with no key is noise too.
#
# The eFuse is the only source of truth for which board is which. Serial ports
# renumber on every replug, and two CP2102 adapters can report an identical USB
# serial, so a port path proves nothing. SPI_BOOT_CRYPT_CNT counts set bits;
# odd (1 or 3) means encryption is on.
#
# Deliberately fails CLOSED. If the eFuse cannot be read, this refuses rather
# than guessing, because guessing wrong is exactly the outcome it exists to
# prevent. Set BRAMBLE_SKIP_ENCRYPTION_CHECK=1 to override, e.g. on a fresh
# board that is not answering yet.
assert_encryption_matches_action() {
  if [[ "${BRAMBLE_SKIP_ENCRYPTION_CHECK:-0}" == "1" ]]; then
    echo "==> WARNING: encryption check skipped (BRAMBLE_SKIP_ENCRYPTION_CHECK=1)"
    return 0
  fi

  echo "==> Reading flash-encryption eFuse on $PORT..."
  local summary crypt_lines state
  if ! summary=$(run_serial_cmd python -m espefuse --port "$PORT" summary 2>/dev/null); then
    echo "flash.sh: could not read eFuses on $PORT, refusing to flash." >&2
    echo "  A plaintext image on a flash-encrypted board bricks it, and this" >&2
    echo "  check is the only thing that tells the two apart." >&2
    echo "  Re-run with BRAMBLE_SKIP_ENCRYPTION_CHECK=1 only if you are certain." >&2
    exit 3
  fi

  # crypt_state_from_summary (scripts/lib/crypt-state.sh) reads the (0b...)
  # value token off the CRYPT_CNT row and applies the odd-parity rule shared
  # with flash-fleet.sh. It echoes "" for a summary with no parseable CRYPT_CNT
  # row, which this refuses rather than assuming safe: the same fail-closed
  # stance as an unreadable eFuse.
  state=$(printf '%s\n' "$summary" | crypt_state_from_summary)
  if [[ -z "$state" ]]; then
    echo "flash.sh: could not parse the flash-encryption eFuse on $PORT, refusing to flash." >&2
    echo "  Expected a SPI_BOOT_CRYPT_CNT / FLASH_CRYPT_CNT row with a (0b...) value; the" >&2
    echo "  summary's matching rows were:" >&2
    crypt_lines=$(printf '%s\n' "$summary" | crypt_cnt_lines)
    if [[ -n "$crypt_lines" ]]; then
      printf '%s\n' "$crypt_lines" | sed 's/^/    /' >&2
    else
      echo "    <no SPI_BOOT_CRYPT_CNT / FLASH_CRYPT_CNT row at all>" >&2
    fi
    echo "  Re-run with BRAMBLE_SKIP_ENCRYPTION_CHECK=1 only if you are certain." >&2
    exit 3
  fi

  if [[ "$state" == "encrypted" && "$ACTION" == "flash" ]]; then
    echo "flash.sh: $PORT has flash encryption enabled; a plaintext flash bricks it." >&2
    echo "  Use: bash scripts/flash.sh local $BOARD encrypted-flash $PORT" >&2
    exit 3
  fi
  if [[ "$state" == "plaintext" && "$ACTION" == "encrypted-flash" ]]; then
    echo "flash.sh: $PORT has no flash-encryption key; an encrypted image is unbootable." >&2
    echo "  Use: bash scripts/flash.sh local $BOARD flash $PORT" >&2
    exit 3
  fi
  echo "==> Encryption check OK (state=$state, action=$ACTION)"
}

run_local() {
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
    flash|encrypted-flash)
      echo "==> Building locally..."
      [[ ! -d "$BOARD_BUILD_DIR" ]] && rm -f "$BOARD_SDKCONFIG"
      idf.py "${IDF_BOARD_ARGS[@]}" "${GLOBAL_ARGS[@]}" build
      # Anti-rollback consent gate: refuses to flash an anti-rollback build
      # unless --enable-antirollback was passed AND the operator types the
      # epoch-specific confirmation. A non-anti-rollback build passes silently.
      bash scripts/antirollback-guard.sh --sdkconfig "$BOARD_SDKCONFIG" --action flash \
        --port "$PORT" ${ANTIROLLBACK_ARGS[@]+"${ANTIROLLBACK_ARGS[@]}"}
      assert_encryption_matches_action
      echo "==> Flashing to $PORT (serial, action: $ACTION)..."
      run_serial_cmd idf.py "${IDF_BOARD_ARGS[@]}" "${GLOBAL_ARGS[@]}" -p "$PORT" "$ACTION" \
        ${FLASH_CMD_ARGS[@]+"${FLASH_CMD_ARGS[@]}"}
      ;;
    *)
      print_usage
      exit 1
      ;;
  esac
}

prepare_local_env
run_local

echo "==> Done!"
