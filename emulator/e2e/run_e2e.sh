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
# stack; this script's trap is a last-resort safety net in case Playwright
# itself is killed before its own teardown runs. It is scoped to ONLY the pid
# this run's globalSetup recorded (emulator/e2e/.run/gosim.pid) -- a
# process-group kill of that one pid, not a name-wide pkill -- so it never
# touches an unrelated gosim/firmware-node instance running the same binary
# on a different port (e.g. a developer's live `make run`).
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

# shellcheck source=emulator/lib/output.sh
source "$REPO_ROOT/emulator/lib/output.sh"

# GOSIM_PID_FILE mirrors lib/stack.ts's PID_FILE: the pid of the gosim THIS
# run's globalSetup spawned (gosim's own process group leader; firmware node
# children inherit that group, see stack.ts's header comment). Normal runs
# never reach this trap with the file still present -- globalTeardown.ts
# already reaped it and removed the file -- so this only fires as a fallback
# when Playwright itself got killed first.
GOSIM_PID_FILE="$E2E_DIR/.run/gosim.pid"

cleanup() {
    if [ -f "$GOSIM_PID_FILE" ]; then
        local pid
        pid="$(cat "$GOSIM_PID_FILE" 2>/dev/null || true)"
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            # Negative pid = kill the whole process group (gosim + any
            # firmware node children), never any unrelated process elsewhere
            # that merely happens to share the binary path.
            kill -TERM "-$pid" 2>/dev/null || true
            sleep 0.2
            kill -KILL "-$pid" 2>/dev/null || true
        fi
        rm -f "$GOSIM_PID_FILE"
    fi
}
trap cleanup EXIT INT TERM

[ -x "$NODE_BIN" ]  || { red "FAIL: node binary missing: $NODE_BIN"; exit 2; }

# Always (re)build gosim: `go build` is itself the staleness check, a near
# no-op when nothing changed (and CI's earlier Build-gosim step keeps it a
# cache hit there). A build-if-missing check here once let a checked-out
# binary from an older commit run as the broker, and the suite then failed
# for reasons that looked like a firmware regression.
( cd "$(dirname "$GOSIM_BIN")" && go build -o "$(basename "$GOSIM_BIN")" . ) \
    || { red "FAIL: gosim build failed"; exit 2; }

[ -d "$UI_DIR/dist" ] || { red "FAIL: UI dist missing: $UI_DIR/dist"; exit 2; }
# The dist bundle must not predate the UI sources: the specs and the bundle
# evolve together (e.g. the canvas paint-history hook the boot-text check
# reads), and a stale bundle fails tests in ways that look like app or
# firmware regressions. Fail loud instead of testing against it. On CI the
# bundle is always built after checkout, so this never fires there.
stale_src="$(find "$UI_DIR/src" "$UI_DIR/index.html" "$UI_DIR/package.json" \
    -newer "$UI_DIR/dist/index.html" -print -quit 2>/dev/null || true)"
[ -z "$stale_src" ] || { red "FAIL: UI dist is older than $stale_src (cd simulator/ui && npm run build)"; exit 2; }
[ -d "$UI_DIR/node_modules/@playwright/test" ] || { red "FAIL: @playwright/test not installed (cd simulator/ui && npm install)"; exit 2; }

# Node resolves bare-specifier imports by walking up from the importing
# file's own directory; emulator/e2e/specs isn't inside simulator/ui, so it
# needs its own node_modules entry pointing at the real one.
ln -sfn "$UI_DIR/node_modules" "$E2E_DIR/node_modules"

info "verifying chromium (playwright)..."
if [ -n "${CI:-}" ]; then
    # CI runners bake the chromium + headless-shell binaries (and their system
    # libraries) into the runner image at PLAYWRIGHT_BROWSERS_PATH
    # (private runner-image definition, image >= 1.2.0), so this REQUIRED
    # suite no longer downloads ~290MB from the playwright CDN or runs apt as
    # root on every run. ASSERT instead of installing: the browser revision is
    # keyed to the playwright version simulator/ui's lockfile resolves, and a
    # silent job-time re-download would hide that the image has drifted from
    # the lockfile. On drift this fails loud; the fix is bumping
    # PLAYWRIGHT_BROWSERS_FOR in the runner image Dockerfile to the new
    # playwright version and rebuilding the image.
    ( cd "$UI_DIR" && node -e '
const fs = require("fs");
const { chromium } = require("playwright-core");
const p = chromium.executablePath();
try { fs.accessSync(p); } catch {
  console.error("FAIL: baked chromium missing at " + p);
  console.error("The runner image does not bake the browser revision this playwright version resolves.");
  console.error("Bump PLAYWRIGHT_BROWSERS_FOR in the private runner-image definition and rebuild the image.");
  process.exit(2);
}
const rev = (p.match(/chromium-(\d+)/) || [])[1];
const shell = process.env.PLAYWRIGHT_BROWSERS_PATH + "/chromium_headless_shell-" + rev;
if (!fs.existsSync(shell)) {
  console.error("FAIL: baked chromium headless shell missing at " + shell);
  console.error("Rebuild the runner image so both chromium and its headless shell are baked for this revision.");
  process.exit(2);
}
console.log("baked chromium OK: " + p);
' )
else
    ( cd "$UI_DIR" && npx playwright install chromium )
fi

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
