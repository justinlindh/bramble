#!/usr/bin/env bash
#
# run_e2e.sh: thin wrapper the `make e2e` target calls. Picks a free port,
# makes sure the specs (which live here, outside simulator/ui) can resolve
# @playwright/test from simulator/ui/node_modules (Node resolves node_modules
# by walking up from each importing FILE's directory, not the invoking
# shell's cwd, so a symlink is the standard fix for a spec tree that lives
# outside the package that owns its devDependencies), makes sure chromium is
# installed, then runs the suite. globalSetup.ts/globalTeardown.ts (see
# playwright.config.ts) own booting and killing the actual gosim+firmware
# stack; this script's trap is a last-resort safety net matching
# emulator/scripts/smoke_live.sh and emulator/ci/run_scenarios.sh, in case
# Playwright itself is killed before its own teardown runs.
#
# Prerequisites (built by the `make e2e` target's node/broker/ui deps):
#   emulator/node/build/bramble-node.elf, simulator/gosim/bramble-gosim,
#   simulator/ui/dist/.
#
# Exit 0 on suite PASS, non-zero otherwise.

set -euo pipefail

E2E_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$E2E_DIR/../.." && pwd)"
UI_DIR="$REPO_ROOT/simulator/ui"
NODE_BIN="$REPO_ROOT/emulator/node/build/bramble-node.elf"
GOSIM_BIN="$REPO_ROOT/simulator/gosim/bramble-gosim"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '  %s\n' "$*"; }

cleanup() {
    pkill -f "$NODE_BIN" 2>/dev/null || true
    pkill -f "$GOSIM_BIN" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

[ -x "$NODE_BIN" ]  || { red "FAIL: node binary missing: $NODE_BIN"; exit 2; }
[ -x "$GOSIM_BIN" ] || { red "FAIL: gosim binary missing: $GOSIM_BIN"; exit 2; }
[ -d "$UI_DIR/dist" ] || { red "FAIL: UI dist missing: $UI_DIR/dist"; exit 2; }
[ -d "$UI_DIR/node_modules/@playwright/test" ] || { red "FAIL: @playwright/test not installed (cd simulator/ui && npm install)"; exit 2; }

# Node resolves bare-specifier imports by walking up from the importing
# file's own directory; emulator/e2e/specs isn't inside simulator/ui, so it
# needs its own node_modules entry pointing at the real one.
ln -sfn "$UI_DIR/node_modules" "$E2E_DIR/node_modules"

info "installing/verifying chromium (playwright)..."
( cd "$UI_DIR" && npx playwright install chromium )

E2E_PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
export E2E_PORT
info "picked free port $E2E_PORT"

echo "=== Bramble emulator browser E2E suite ==="
info "repo:   $REPO_ROOT"
info "port:   $E2E_PORT"
info "config: $E2E_DIR/playwright.config.ts"
echo

cd "$UI_DIR"
if npx playwright test --config "$E2E_DIR/playwright.config.ts"; then
    green "=== E2E SUITE PASS ==="
    exit 0
fi
red "=== E2E SUITE FAIL ==="
exit 1
