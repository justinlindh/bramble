#!/usr/bin/env bash
# Anti-regression gate: fails if the tracked tree carries references to the
# maintainer's private infrastructure, home network, or bench hardware. A
# public repo must never carry these. Scans ALL tracked files including
# markdown. This gate was hardened after an audit found leak classes the
# original missed (MACs, real device addresses, home room names).
#
# Allowed: the ESP32 SoftAP default gateway range 192.168.4.x (a product
# constant), the RFC1918 range definitions inside the proxy target-policy
# source and the tests that exercise it, and clearly-fake documentation
# placeholders (RFC5737 IPs 192.0.2.x etc., MAC AA:BB:CC:..., hex
# DEADBEEF/CAFEBABE...).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

self="scripts/lint/check-no-internal-refs.sh"
fail=0
report() { printf 'check-no-internal-refs: %s:\n%s\n' "$1" "$2" >&2; fail=1; }

# 1. Internal hostnames, fleet secrets dir, personal absolute paths.
h="$(git grep -nIE 'idiotica|dumbot|openclaw|bramble-meta|/home/justin' -- . ":!${self}" || true)"
[[ -n "$h" ]] && report "internal infra references" "$h"

# 2. Private RFC1918 addresses: 10.0.0.0/8, 172.16.0.0/12 (172.16 through
#    172.31 only, so 172.15.x and 172.32.x stay public) and 192.168.0.0/16.
#    The ESP32 SoftAP 192.168.4.x is carved out in the pattern itself rather
#    than by dropping whole lines, so an AP address on a line cannot mask a
#    real leak next to it. All four octets are required and the match is
#    bounded by non-digit, non-dot characters, which keeps dotted version
#    strings such as 10.0.0 or 1.10.0.0.1 from matching. The proxy
#    target-policy source defines these ranges and its tests plus the
#    unified-server proxy tests must feed it real RFC1918 targets.
rfc1918='(^|[^0-9.])(10(\.[0-9]{1,3}){3}'
rfc1918+='|172\.(1[6-9]|2[0-9]|3[01])(\.[0-9]{1,3}){2}'
rfc1918+='|192\.168\.([0-35-9]|[0-9]{2,3})\.[0-9]{1,3})([^0-9.]|$)'
h="$(git grep -nIE "$rfc1918" -- . ":!${self}" \
  ':!webapp/server/target-policy.mjs' \
  ':!webapp/server/target-policy.test.mjs' \
  ':!webapp/server/unified-server.test.mjs' || true)"
[[ -n "$h" ]] && report "private RFC1918 address (outside the ESP32 AP range)" "$h"

# 3. Real bench-hardware node addresses (Ed25519-derived 4-byte addresses).
#    Public docs/tests must use fake hex (DEADBEEF, CAFEBABE, ...).
h="$(git grep -niIE 'F2BE6EEE|AB246C7C|FEC61437|9CA6A0EE|50D2E1BD|4A555354' -- . ":!${self}" || true)"
[[ -n "$h" ]] && report "real bench device address" "$h"

# 4. Home room / location labels used as device names in fixtures.
h="$(git grep -nIE "\b(Garage|Attic)\b" -- . ":!${self}" || true)"
[[ -n "$h" ]] && report "home room/location label" "$h"

# 5. MAC addresses, except clearly-fake documentation placeholders.
h="$(git grep -nIE '([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}' -- . ":!${self}" | grep -viE 'AA:BB:CC:DD:EE|00:00:00:00:00:00|DE:AD:BE:EF|FF:FF:FF:FF:FF:FF' || true)"
[[ -n "$h" ]] && report "MAC address (use a documentation placeholder like AA:BB:CC:DD:EE:FF)" "$h"


# 6b. Home location: neighborhood/city names and precise coordinates from the
#     maintainer's real area. Public fixtures must use fictional places.
h="$(git grep -nIE 'Inspirada|Henderson|McCullough|Sloan Canyon|Anthem|35\.9[0-9]{4}|36\.0[0-9]{3}|-11[45]\.[0-9]{3,}|openstreetmap\.org/\?mlat' -- . ":!${self}" ':!hardware/' || true)"
[[ -n "$h" ]] && report "home location / neighborhood / coordinates" "$h"

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "check-no-internal-refs: clean"
