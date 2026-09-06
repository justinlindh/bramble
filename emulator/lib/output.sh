# shellcheck shell=bash
#
# Shared terminal-output helpers for the emulator's entry-point scripts
# (ci/run_scenarios.sh, scripts/smoke_live.sh, e2e/run_e2e.sh). Each of those
# had its own byte-identical copy of these three one-liners; this is the single
# definition they all source so the suite's PASS/FAIL colouring and indent stay
# consistent as the scripts grow.

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '  %s\n' "$*"; }
