#!/usr/bin/env bash
# Build qemu-system-xtensa from source with the Bramble device-model
# injection scaffold applied. QEMU has no stable plugin API for adding
# devices, so P2.2-P2.5's GPIO/GPSPI2/SX1262/SSD1680 models require a
# from-source build; this script stands up that pipeline. See README.md
# ("Build QEMU from source").
#
# Idempotent: safe to re-run. Skips the clone if $QEMU_SRC already has the
# pinned tag checked out, skips the patch apply if already applied, skips
# configure if already configured, and ninja no-ops when nothing changed.
#
# Usage:
#   emulator/qemu/bootstrap-qemu.sh
#
# Env:
#   QEMU_SRC   where to clone/build espressif/qemu (default ~/src/qemu-esp)
set -euo pipefail

QEMU_TAG="esp-develop-9.2.2-20260417"
QEMU_REPO="https://github.com/espressif/qemu"
QEMU_SRC="${QEMU_SRC:-$HOME/src/qemu-esp}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODELS_DIR="$SCRIPT_DIR/models"
PATCHES_DIR="$SCRIPT_DIR/patches"

log() { echo "bootstrap-qemu: $*" >&2; }
die() { echo "bootstrap-qemu: FATAL: $*" >&2; exit 1; }

# --- clone (or reuse) ---
if [[ -d "$QEMU_SRC/.git" ]]; then
    log "reusing existing clone at $QEMU_SRC"
else
    log "cloning $QEMU_REPO @ $QEMU_TAG into $QEMU_SRC"
    git clone --branch "$QEMU_TAG" --depth 1 "$QEMU_REPO" "$QEMU_SRC" \
        || die "clone of $QEMU_REPO @ $QEMU_TAG failed"
fi

cd "$QEMU_SRC"
CURRENT_TAG="$(git describe --tags 2>/dev/null || true)"
if [[ "$CURRENT_TAG" != "$QEMU_TAG" ]]; then
    die "checked-out tag ('$CURRENT_TAG') != pinned ('$QEMU_TAG') at $QEMU_SRC; remove it and re-run to get a clean checkout"
fi

# --- inject model sources ---
# .c files and the bramble-owned meson.build land in hw/xtensa/bramble/ (the
# subdir the one meson patch reaches with subdir('bramble')); .h files land
# where the esp32s3 machine's sibling headers live (include/hw/xtensa/). See
# models/README.md. Only copies when content differs, so a no-op re-run does not
# touch mtimes and trigger a spurious ninja rebuild.
BRAMBLE_DIR="hw/xtensa/bramble"
mkdir -p "$BRAMBLE_DIR"
log "injecting model sources from $MODELS_DIR"
shopt -s nullglob
inject() {
    local src="$1" dst="$2"
    cmp -s "$src" "$dst" 2>/dev/null || cp "$src" "$dst" || die "failed to inject $(basename "$src")"
}
for f in "$MODELS_DIR"/*.c; do
    inject "$f" "$BRAMBLE_DIR/$(basename "$f")"
done
inject "$MODELS_DIR/meson.build" "$BRAMBLE_DIR/meson.build"
for f in "$MODELS_DIR"/*.h; do
    inject "$f" "include/hw/xtensa/$(basename "$f")"
done
shopt -u nullglob

# --- apply wiring patches (idempotency guard: skip if already applied) ---
for p in "$PATCHES_DIR"/*.patch; do
    [[ -e "$p" ]] || continue
    name="$(basename "$p")"
    if git apply --check "$p" 2>/dev/null; then
        log "applying $name"
        git apply "$p" || die "$name failed to apply after passing --check (race?)"
    elif git apply --reverse --check "$p" 2>/dev/null; then
        log "$name already applied, skipping"
    else
        die "$name does not apply cleanly and does not look already-applied; tree may have diverged from $QEMU_TAG"
    fi
done

# --- configure ---
if [[ ! -f build/build.ninja ]]; then
    log "configuring (xtensa-softmmu, slirp off, werror off, gnutls off)"
    mkdir -p build
    # --disable-gnutls: this tag's top-level meson.build only probes for
    # libgcrypt when gnutls's own crypto backend is NOT found (gnutls,
    # nettle, gcrypt are alternative backends for QEMU's own TLS/migration
    # crypto and normally only one is needed). This host has gnutls, so
    # without this flag gcrypt is never even probed even though it is
    # present (pkg-config finds it fine standalone) and the esp32s3
    # AES/RSA/DS/XTS_AES device models, which are gated on `gcrypt.found()`
    # in hw/misc/meson.build and are unconditionally instantiated by the
    # esp32s3 machine regardless of whether the firmware under test uses
    # them, silently fail to compile in: qemu-system-xtensa then aborts at
    # machine realize with "unknown type 'misc.esp32s3.aes'". We do not
    # need gnutls for anything (chardev sockets, no TLS), so disabling it
    # is the minimal fix rather than pulling in gcrypt some other way.
    (cd build && ../configure --target-list=xtensa-softmmu \
        --disable-slirp --disable-werror --disable-gnutls) \
        || die "configure failed"
else
    log "build/ already configured, skipping configure"
fi

# --- pin werror off in the native file (GCC-16 workaround) ---
# GCC 16 promotes const-discard warnings in this pinned QEMU's stock code
# (e.g. util/qemu-sockets.c) to errors under -Werror. `configure
# --disable-werror` records werror=false in cmd_line.txt (so the initial
# build.ninja is generated with -Werror off), but configure still writes
# `werror = true` into the generated native file config-meson.cross. That
# native-file value wins on any later meson *reconfigure* (which ninja
# triggers automatically whenever a meson.build changes, e.g. when a new
# device-model patch lands), silently reintroducing -Werror and breaking an
# otherwise-clean incremental build. Pin it off in the native file so the
# setting survives regeneration.
CROSS="$QEMU_SRC/build/config-meson.cross"
if [[ -f "$CROSS" ]] && grep -q '^werror = true' "$CROSS"; then
    log "pinning werror=false in config-meson.cross (GCC-16 -Werror workaround)"
    sed -i 's/^werror = true/werror = false/' "$CROSS"
fi

# --- build ---
log "building with ninja (-j$(nproc))"
(cd build && ninja -j"$(nproc)") || die "ninja build failed"

BIN="$QEMU_SRC/build/qemu-system-xtensa"
[[ -x "$BIN" ]] || die "ninja reported success but $BIN is missing"
log "build OK"
echo "$BIN"
