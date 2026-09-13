#!/usr/bin/env bash
# Single source of truth for the cppcheck gate. Both `make ci-quality-cppcheck`
# and the Static checks job in .github/workflows/firmware-quality.yml invoke
# this script, so the exact flag set (the enabled checks and every scoped
# suppression below) cannot drift between a local `make ci` and CI.
set -u

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "[cppcheck] FAIL: cppcheck not found. Install cppcheck to run this gate." >&2
  exit 1
fi

# The analysis roots and suppressions are repo-relative, so run from the repo
# root regardless of the caller's working directory.
repo_root="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
cd "$repo_root" || exit 1

# The scoped suppressions are cppcheck 2.13 false positives (the previous runner
# shipped 2.7, which did not flag them): unknownMacro fires because the lvgl
# headers are not on the include path (LV_SYMBOL_RIGHT), and both uninitvar
# findings are loops that only read entries below a count that starts at zero.
exec cppcheck \
  --enable=warning,performance,portability \
  --std=c11 \
  --quiet \
  --error-exitcode=2 \
  --suppress=normalCheckLevelMaxBranches \
  --suppress=unknownMacro:components/ui_graphics/screens/scr_chat_messages.c \
  --suppress=uninitvar:components/ui_graphics/screens/scr_map.c \
  --suppress=uninitvar:components/ui_graphics/screens/scr_nodes.c \
  main components
