#!/usr/bin/env bash
#
# check_prereqs.sh: verify the toolchain needed to build and run the Bramble
# emulator (the Makefile `check` target). Prints what is present, what is
# missing, and how to get anything missing. Exits nonzero if the emulator
# cannot be built/run with the current toolchain.
#
# ESP-IDF location: discovered by scripts/ci-source-idf.sh, the same helper CI
# uses, so a checkout the pipeline can build is one this check passes. It takes
# IDF_PATH when set, an idf.py already on PATH otherwise, and failing both it
# walks the install locations that script lists.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IDF_SOURCE_SH="$REPO_ROOT/scripts/ci-source-idf.sh"

missing=0

pass() { printf '  OK       %s\n' "$*"; }
note() { printf '  NOTE     %s\n' "$*"; }
bad()  { printf '  MISSING  %s\n' "$*"; missing=1; }

echo "=== Bramble emulator prerequisite check ==="

# --- ESP-IDF / idf.py, linux preview target -------------------------------
# Probed in a subshell: sourcing export.sh rewrites PATH and a pile of
# environment, and a check has no business leaving that behind in the caller's
# shell. It reports the version and the IDF_PATH the helper settled on, so a
# machine with several toolchains says which one the build will use.
idf_probe() {
    # shellcheck disable=SC1090
    source "$IDF_SOURCE_SH" >/dev/null 2>&1 || return 1
    command -v idf.py >/dev/null 2>&1 || return 1
    printf '%s (IDF_PATH=%s)' "$(idf.py --version 2>/dev/null)" "${IDF_PATH:-unset}"
}

if [ ! -f "$IDF_SOURCE_SH" ]; then
    bad "ESP-IDF locator missing: $IDF_SOURCE_SH"
elif idf_desc=$(idf_probe); then
    pass "idf.py $idf_desc, linux preview target"
else
    bad "ESP-IDF not found: no idf.py on PATH and no export.sh in any known location"
    note "set IDF_PATH=/path/to/esp-idf, or clone ESP-IDF 5.4.1 and install the"
    note "linux target: git clone -b v5.4.1 --recurse-submodules \\"
    note "  https://github.com/espressif/esp-idf.git ~/esp-idf && \\"
    note "  ~/esp-idf/install.sh linux"
    note "the locations searched are listed by: bash $IDF_SOURCE_SH"
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
