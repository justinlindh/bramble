#!/usr/bin/env bash
# Configure ccache for an ESP-IDF build job.
#
# ESP-IDF has native ccache support: idf.py reads IDF_CCACHE_ENABLE and, when
# it is set, puts ccache in front of the cross compiler via
# CMAKE_C_COMPILER_LAUNCHER. This script exports that switch plus a cache
# directory, and emits step outputs so the calling workflow can wire up
# actions/cache with a key that includes the ESP-IDF version.
#
# ccache is a build accelerator, not a gate: if the runner image does not ship
# ccache, this script emits available=false and the build proceeds uncached
# rather than failing the job.
#
# Step outputs (GITHUB_OUTPUT):
#   available    "true" when ccache is present and was enabled, else "false"
#   dir          the CCACHE_DIR path to cache
#   idf_version  the ESP-IDF version string, sanitized for use in a cache key
#
# Environment written to GITHUB_ENV when available:
#   IDF_CCACHE_ENABLE, CCACHE_DIR, CCACHE_MAXSIZE, CCACHE_BASEDIR,
#   CCACHE_COMPILERCHECK

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

emit() {
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$1" "$2" >> "$GITHUB_OUTPUT"
  else
    printf '[ci-ccache] output %s=%s\n' "$1" "$2"
  fi
}

export_env() {
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf '%s=%s\n' "$1" "$2" >> "$GITHUB_ENV"
  fi
  export "$1"="$2"
}

# The build steps override HOME to a scratch path, so ccache's default
# ~/.ccache would land somewhere different per step and never be cacheable.
# RUNNER_TEMP survives every step of a job and is outside the checkout, so
# actions/checkout's clean cannot touch it.
CCACHE_ROOT="${RUNNER_TEMP:-/tmp}/bramble-ccache"

if ! command -v ccache >/dev/null 2>&1; then
  echo "::notice::ccache is not installed on this runner image; building without a compiler cache"
  emit available "false"
  emit dir "$CCACHE_ROOT"
  emit idf_version "unknown"
  exit 0
fi

# Resolve the ESP-IDF version so the cache key changes when the toolchain
# changes. Objects compiled by a different compiler must never be reused.
IDF_VERSION="unknown"
if command -v idf.py >/dev/null 2>&1 || [[ -f "$SCRIPT_DIR/ci-source-idf.sh" ]]; then
  # shellcheck disable=SC1091
  if source "$SCRIPT_DIR/ci-source-idf.sh" >/dev/null 2>&1; then
    IDF_VERSION="$(idf.py --version 2>/dev/null | tr -d '\n' || true)"
  fi
fi
if [[ -z "$IDF_VERSION" ]]; then
  IDF_VERSION="unknown"
fi
# Cache keys must not contain commas or whitespace.
IDF_VERSION="$(printf '%s' "$IDF_VERSION" | tr -c '[:alnum:].-' '_')"

mkdir -p "$CCACHE_ROOT"

export_env IDF_CCACHE_ENABLE "1"
export_env CCACHE_DIR "$CCACHE_ROOT"
export_env CCACHE_MAXSIZE "${BRAMBLE_CCACHE_MAXSIZE:-2G}"
# Hash compiler contents rather than mtime: the toolchain lives in a baked
# image layer whose timestamps are not stable across pods.
export_env CCACHE_COMPILERCHECK "content"
# Build paths are absolute; rewriting them relative to the workspace keeps
# hits across pods whose workspace path differs.
export_env CCACHE_BASEDIR "${GITHUB_WORKSPACE:-$PWD}"

echo "[ci-ccache] enabled: dir=$CCACHE_ROOT idf_version=$IDF_VERSION"
ccache --version | head -n 1

emit available "true"
emit dir "$CCACHE_ROOT"
emit idf_version "$IDF_VERSION"
