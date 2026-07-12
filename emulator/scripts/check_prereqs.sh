#!/usr/bin/env bash
#
# check_prereqs.sh: verify the toolchain needed to build and run the Bramble
# emulator (the Makefile `check` target). Prints what is present, what is
# missing, and how to get anything missing. Exits nonzero if the emulator
# cannot be built/run with the current toolchain.
#
# ESP-IDF location: set IDF_PATH, or it defaults to ~/src/esp-idf.

set -u

IDF_PATH="${IDF_PATH:-$HOME/src/esp-idf}"

missing=0

pass() { printf '  OK       %s\n' "$*"; }
note() { printf '  NOTE     %s\n' "$*"; }
bad()  { printf '  MISSING  %s\n' "$*"; missing=1; }

echo "=== Bramble emulator prerequisite check ==="

# --- ESP-IDF / idf.py, linux preview target -------------------------------
if [ ! -f "$IDF_PATH/export.sh" ]; then
    bad "ESP-IDF not found at $IDF_PATH/export.sh"
    note "set IDF_PATH=/path/to/esp-idf, or clone ESP-IDF 5.4 and install the"
    note "linux target: git clone -b v5.4 --recurse-submodules \\"
    note "  https://github.com/espressif/esp-idf.git ~/src/esp-idf && \\"
    note "  ~/src/esp-idf/install.sh linux"
elif ver=$(bash -c "source '$IDF_PATH/export.sh' >/dev/null 2>&1 && idf.py --version" 2>/dev/null); then
    pass "idf.py ($ver, IDF_PATH=$IDF_PATH, linux preview target)"
else
    bad "idf.py did not initialize from $IDF_PATH/export.sh"
    note "run: $IDF_PATH/install.sh linux"
fi

# --- go ---------------------------------------------------------------------
if command -v go >/dev/null 2>&1; then
    pass "go ($(go version))"
else
    bad "go (https://go.dev/dl/)"
fi

# --- node / npm ---------------------------------------------------------------
if command -v node >/dev/null 2>&1; then
    pass "node ($(node --version))"
else
    bad "node (https://nodejs.org, or via nvm/mise)"
fi

if command -v npm >/dev/null 2>&1; then
    pass "npm ($(npm --version))"
else
    bad "npm (bundled with node; reinstall node)"
fi

# --- jq -----------------------------------------------------------------------
if command -v jq >/dev/null 2>&1; then
    pass "jq ($(jq --version))"
else
    bad "jq (apt install jq / brew install jq / pacman -S jq)"
fi

# --- docker (optional) ---------------------------------------------------------
if command -v docker >/dev/null 2>&1; then
    pass "docker (optional: enables 'docker compose up --build' zero-prerequisite path)"
else
    note "docker not found (optional; only needed for 'docker compose up --build')"
fi

echo
if [ "$missing" -ne 0 ]; then
    echo "prerequisites missing, see MISSING lines above"
    exit 1
fi
echo "all required prerequisites present"
