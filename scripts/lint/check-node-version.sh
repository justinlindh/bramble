#!/usr/bin/env bash
# Anti-drift gate: every Node major in the tree must match the single source of
# truth in `.nvmrc`.
#
# Why this exists: the shipped artifacts silently ended up on three different
# Node majors. `.nvmrc` and every CI pin said 20 (end of life since
# 2026-04-30), webapp/Dockerfile built and ran the published container image on
# 26, the desktop installer job inherited whatever the electronuserland/builder
# image shipped, and the compose services used 22. Nobody chose that spread, and
# nothing failed when it appeared, which is exactly the failure mode this check
# closes.
#
# Moving Node is a deliberate act: edit `.nvmrc`, then update every file this
# script names. The major lives in exactly one place, so a bump never means
# editing this script.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# shellcheck source=scripts/lib/version-check.sh
source scripts/lib/version-check.sh

self="scripts/lint/check-node-version.sh"
sot_file=".nvmrc"
name="check-node-version"
fail=0

want="$(vc_read_sot "$name" "$sot_file")"
if [[ ! "$want" =~ ^[0-9]+$ ]]; then
  echo "check-node-version: $sot_file must hold a bare Node major, got '$want'" >&2
  exit 1
fi

# 1. Required pins. Each entry is "<file>::<literal that must appear>". These
#    are the references that matter: the ones that decide which Node actually
#    builds or runs a shipped artifact, plus the prose contributors follow. A
#    pattern sweep alone cannot catch a line that drops its Node context, so
#    assert the exact expected text.
required=(
  "webapp/Dockerfile::FROM --platform=\$BUILDPLATFORM node:${want}-slim AS build"
  "webapp/Dockerfile::FROM node:${want}-slim AS runtime"
  "simulator/Dockerfile::FROM node:${want}-slim AS ui-build"
  "emulator/Dockerfile::FROM node:${want}-slim AS ui-build"
  "webapp/package.json::\"node\": \">=${want}\""
  "CONTRIBUTING.md::Node.js ${want} or newer"
  "webapp/README.md::Node.js ${want} or newer"
  "web-flasher/README.md::Use Node ${want} (the CI version"
  # The desktop installer container leg has no setup-node step: the image tag
  # IS its Node pin, so the sweep in section 2 cannot see it. electron-builder
  # publishes a <major>-wine variant per Node base; the unqualified :wine tag
  # floats onto whatever base is newest, which is how this leg ended up
  # building the Linux and Windows installers on a Node major nobody chose.
  ".github/workflows/release-components.yml::image: electronuserland/builder:${want}-wine"
)
vc_require_pins "$name" "Node ${want}" "${required[@]}" || fail=1

# 2. Sweep every actions/setup-node pin in the authoritative workflow tree.
#    A `node-version:` that disagrees with .nvmrc means CI tests on a runtime
#    nothing else uses.
setup_node_hits="$(git grep -nE '^[[:space:]]*node-version:' -- '.github/workflows' \
                   | grep -vE ":[[:space:]]*node-version:[[:space:]]*'?${want}'?[[:space:]]*$" || true)"
if [[ -n "$setup_node_hits" ]]; then
  vc_report "$name" "actions/setup-node pin does not match ${sot_file} (${want}):"
  printf '%s\n' "$setup_node_hits" >&2
  fail=1
fi

# 3. Sweep every Node base image in the tracked Dockerfiles and compose files.
#    The leading boundary keeps `bramble/idf-node:v5.4.1` and `node:test` out.
image_hits="$(git grep -nE '(^|[^-[:alnum:]])node:[0-9]+' \
                -- 'Dockerfile' '*/Dockerfile' '**/Dockerfile' 'docker-compose*.yml' '**/docker-compose*.yml' \
                ":!${self}" \
              | grep -vE "(^|[^-[:alnum:]])node:${want}[-.[:space:]\"']" || true)"
if [[ -n "$image_hits" ]]; then
  vc_report "$name" "Node base image does not match ${sot_file} (${want}):"
  printf '%s\n' "$image_hits" >&2
  fail=1
fi

if (( fail )); then
  echo "check-node-version: FAIL. Source of truth is ${sot_file} (${want}). Bumping Node means editing ${sot_file} plus every file named above." >&2
  exit 1
fi

echo "check-node-version: OK (all Node references pinned to major ${want})"
