#!/usr/bin/env bash
# Anti-regression gate: fails if the tracked tree carries references to the
# maintainer's private infrastructure, home network, or bench hardware. A
# public repo must never carry these. Scans ALL tracked files including
# markdown. This gate was hardened after an audit found leak classes the
# original missed (MACs, real device addresses, home room names).
#
# Allowed: the ESP32 SoftAP default gateway range 192.168.4.x (a product
# constant), the RFC1918 range definition 192.168.0.0/16 inside the proxy
# target-policy source, and clearly-fake documentation placeholders
# (RFC5737 IPs 192.0.2.x etc., MAC AA:BB:CC:..., hex DEADBEEF/CAFEBABE...).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

self="scripts/lint/check-no-internal-refs.sh"
fail=0
report() { printf 'check-no-internal-refs: %s:\n%s\n' "$1" "$2" >&2; fail=1; }

# 1. Internal hostnames, fleet secrets dir, personal absolute paths.
h="$(git grep -nIE 'example|justinlindh|host|internal-planning|/home/user' -- . ":!${self}" || true)"
[[ -n "$h" ]] && report "internal infra references" "$h"

# 2. Private 192.168.x.x except the ESP32 SoftAP 192.168.4.x. The proxy
#    target-policy source legitimately defines the RFC1918 range.
h="$(git grep -nIE '192\.168\.[0-9]+\.[0-9]+' -- . ":!${self}" ':!webapp/server/target-policy.mjs' | grep -vE '192\.168\.4\.' || true)"
[[ -n "$h" ]] && report "private LAN address (outside the ESP32 AP range)" "$h"

# 3. Real bench-hardware node addresses (Ed25519-derived 4-byte addresses).
#    Public docs/tests must use fake hex (DEADBEEF, CAFEBABE, ...).
h="$(git grep -niIE 'DEADBEEF|CAFEBABE|FEEDFACE|D15EA5ED|C0FFEE00' -- . ":!${self}" || true)"
[[ -n "$h" ]] && report "real bench device address" "$h"

# 4. Home room / location labels used as device names in fixtures.
h="$(git grep -nIE "\b(Node A|Node B)\b" -- . ":!${self}" || true)"
[[ -n "$h" ]] && report "home room/location label" "$h"

# 5. MAC addresses, except clearly-fake documentation placeholders.
h="$(git grep -nIE '([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}' -- . ":!${self}" | grep -viE 'AA:BB:CC:DD:EE|00:00:00:00:00:00|DE:AD:BE:EF|FF:FF:FF:FF:FF:FF' || true)"
[[ -n "$h" ]] && report "MAC address (use a documentation placeholder like AA:BB:CC:DD:EE:FF)" "$h"

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "check-no-internal-refs: clean"
