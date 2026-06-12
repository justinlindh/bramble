#!/usr/bin/env bash
# Ensure keys/ota_signing_key.pem exists before an idf.py build.
#
# The build signs bramble.bin with this key (CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES).
# Resolution order:
#   1. keys/ota_signing_key.pem already present: keep it.
#   2. $BRAMBLE_OTA_SIGNING_KEY set: copy that key into place.
#   3. CI: fail. Release builds must be signed with the OTA_SIGNING_KEY repo
#      secret, never a generated key.
#   4. Otherwise: generate a throwaway RSA-3072 dev key and warn. Devices
#      flashed over USB with the resulting build will only accept OTA images
#      signed with this same key.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_PATH="$ROOT_DIR/keys/ota_signing_key.pem"

log() {
  echo "[ensure-ota-signing-key] $*"
}

if [[ -f "$KEY_PATH" ]]; then
  log "Signing key present: $KEY_PATH"
  exit 0
fi

mkdir -p "$ROOT_DIR/keys"

if [[ -n "${BRAMBLE_OTA_SIGNING_KEY:-}" ]]; then
  if [[ ! -f "$BRAMBLE_OTA_SIGNING_KEY" ]]; then
    echo "[ensure-ota-signing-key] ERROR: BRAMBLE_OTA_SIGNING_KEY points to a missing file: $BRAMBLE_OTA_SIGNING_KEY" >&2
    exit 1
  fi
  install -m 600 "$BRAMBLE_OTA_SIGNING_KEY" "$KEY_PATH"
  log "Installed signing key from BRAMBLE_OTA_SIGNING_KEY"
  exit 0
fi

if [[ -n "${CI:-}" || -n "${GITHUB_ACTIONS:-}" ]]; then
  echo "[ensure-ota-signing-key] ERROR: no signing key in CI. The workflow must write the OTA_SIGNING_KEY secret to $KEY_PATH before building." >&2
  exit 1
fi

log "WARNING: generating a throwaway dev signing key at $KEY_PATH"
log "WARNING: devices flashed with this build will only accept OTA images signed with this key"
log "Set BRAMBLE_OTA_SIGNING_KEY to reuse a stable dev key across checkouts"
umask 077
openssl genrsa -out "$KEY_PATH" 3072 2>/dev/null
log "Dev signing key generated"
