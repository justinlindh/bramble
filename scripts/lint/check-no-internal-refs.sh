#!/usr/bin/env bash
# Anti-regression gate: fails if the tracked tree carries references to this
# project's private infrastructure -- the internal Gitea org/host, the
# maintainer's home directory, secret file paths, or any real private LAN
# address. A public repo must never carry these. Scans ALL tracked files,
# including markdown (public docs must be clean too).
#
# The only private-range address allowed is the ESP32 SoftAP default gateway
# range 192.168.4.x, which is a product constant, not a real host. Example
# addresses elsewhere must use the RFC5737 documentation ranges (192.0.2.x,
# 198.51.100.x, 203.0.113.x). The three webapp/server proxy files that
# implement the RFC1918 private-range policy legitimately reference
# 192.168.0.0/16 and are allowlisted for the IP check only.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

self="scripts/lint/check-no-internal-refs.sh"
fail=0

# Internal hostnames, fleet secrets dir, personal absolute paths.
words="$(git grep -nIE 'example|justinlindh|host|internal-planning|/home/user' -- . ":!${self}" || true)"
if [[ -n "$words" ]]; then
  printf 'check-no-internal-refs: internal infra references found:\n%s\n' "$words" >&2
  fail=1
fi

# Any private 192.168.x.x except the ESP32 SoftAP range 192.168.4.x.
# The proxy target-policy files implement the RFC1918 range check itself.
ips="$(git grep -nIE '192\.168\.[0-9]+\.[0-9]+' -- . \
  ":!${self}" \
  ':!webapp/server/target-policy.mjs' \
  ':!webapp/server/target-policy.test.mjs' \
  ':!webapp/server/unified-server.test.mjs' \
  | grep -vE '192\.168\.4\.' || true)"
if [[ -n "$ips" ]]; then
  printf 'check-no-internal-refs: private LAN address (outside the ESP32 AP range) found:\n%s\n' "$ips" >&2
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "check-no-internal-refs: clean"
