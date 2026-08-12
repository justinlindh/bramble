#!/usr/bin/env bash
# Anti-drift gate: every ESP-IDF version reference in the tree must match the
# single source of truth in `.esp-idf-version`.
#
# Why this exists: `v5.4` is a moving release *branch* and `v5.4.1` is a *tag*
# on it. For months the docs contributors follow (README, BUILDING.md) told
# them to clone the branch while CI, the firmware-builder container, and the
# published runner image all pinned the tag, so contributors built firmware
# against a different ESP-IDF than CI compiled with. See issue #165.
#
# Bumping ESP-IDF is a deliberate act: edit `.esp-idf-version`, then update
# every file this script names. The version string itself lives in exactly one
# place, so a bump never means editing this script, only the pinned files it
# points at. That keeps the source of truth a data file rather than a fourth
# copy hidden in lint code.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

self="scripts/lint/check-idf-version.sh"
sot_file=".esp-idf-version"
fail=0
report() { printf 'check-idf-version: %s\n' "$1" >&2; fail=1; }

if [[ ! -f "$sot_file" ]]; then
  echo "check-idf-version: missing source of truth file $sot_file" >&2
  exit 1
fi

# `v5.4.1` (tag form) and `5.4.1` (bare form, used in prose and comments).
want="$(tr -d '[:space:]' < "$sot_file")"
if [[ ! "$want" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "check-idf-version: $sot_file must hold a full vMAJOR.MINOR.PATCH tag, got '$want'" >&2
  exit 1
fi
bare="${want#v}"
major="${bare%%.*}"

# 1. Required pins. Each entry is "<file>::<literal that must appear>". These
#    are the references that matter: the ones that decide which ESP-IDF a
#    build actually uses. A grep-based sweep alone cannot catch a line that
#    drops its ESP-IDF context word, so assert the exact expected text.
required=(
  "docker/firmware-builder/Dockerfile::FROM espressif/idf:${want}"
  "emulator/Dockerfile::FROM espressif/idf:${want}"
  "docs/BUILDING.md::git clone --depth 1 -b ${want} https://github.com/espressif/esp-idf.git"
  "emulator/scripts/check_prereqs.sh::git clone -b ${want} --recurse-submodules"
  "scripts/ci-ensure-idf.sh::ESP-IDF ${want}"
  "CLAUDE.md::ESP-IDF ${want}"
  "CONTRIBUTING.md::ESP-IDF ${want}"
  "README.md::ESP-IDF ${want}"
  "docs/ci/idf-node-runner-image.md::espressif/idf:${want}"
  "docs/ci/idf-node-runner-image.md::bramble/idf-node:${want}"
)
for entry in "${required[@]}"; do
  file="${entry%%::*}"
  literal="${entry#*::}"
  if [[ ! -f "$file" ]]; then
    report "required pin file is missing: $file"
    continue
  fi
  if ! grep -qF -- "$literal" "$file"; then
    report "$file no longer pins ESP-IDF ${want} (expected to find: ${literal})"
  fi
done

# 2. Tree-wide sweep. Any line that mentions ESP-IDF and carries a version
#    token on the pinned major line must name the pinned version exactly.
#    Scoped to major ${major} so unrelated versions on the same line (qemu
#    9.0.0, TypeScript 5.x in lockfiles) do not produce noise.
hits="$(git grep -nIiE "(esp-?idf|espressif/idf|IDF_VERSION|idf-node|idf_tools|\bIDF\b)" \
          -- . ":!${self}" ':!webapp/package-lock.json' ':!**/node_modules/**' \
        | grep -E "(^|[^0-9.])v?${major}\.[0-9]+(\.[0-9]+)?([^0-9.]|$)" \
        | grep -vE "(^|[^0-9.])v?${bare//./\\.}([^0-9.]|$)" || true)"
if [[ -n "$hits" ]]; then
  report "ESP-IDF version reference does not match ${sot_file} (${want}):"
  printf '%s\n' "$hits" >&2
fi

if (( fail )); then
  echo "check-idf-version: FAIL. Source of truth is ${sot_file} (${want})." >&2
  exit 1
fi

echo "check-idf-version: OK (all ESP-IDF references pinned to ${want})"
