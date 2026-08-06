#!/usr/bin/env bash
# RPC contract check: the OpenAPI spec must list exactly the RPC methods the
# firmware registers, no more, no less.
#
# Ground truth (firmware): rpc_register("...") calls in main/rpc_methods.c.
# Spec: /rpc/<method> path keys in api/openapi.yaml.
#
# Fails (exit 1) on any asymmetric difference, and on duplicate path keys in
# the spec (YAML silently keeps only the last duplicate, so a duplicate is a
# latent contract bug even when the names match).
set -euo pipefail

cd "$(dirname "$0")/.."

FIRMWARE_SRC="main/rpc_methods.c"
SPEC="api/openapi.yaml"

for f in "$FIRMWARE_SRC" "$SPEC"; do
    if [ ! -f "$f" ]; then
        echo "::error::$f not found (run from the repo root)" >&2
        exit 1
    fi
done

fw_methods=$(grep -o 'rpc_register("[^"]*"' "$FIRMWARE_SRC" | sed 's/rpc_register("//;s/"$//' | sort -u)
spec_methods_all=$(grep -oE '^  /rpc/[A-Za-z0-9_.]+:' "$SPEC" | sed 's|^  /rpc/||;s/:$//' | sort)
spec_methods=$(printf '%s\n' "$spec_methods_all" | sort -u)

if [ -z "$fw_methods" ]; then
    echo "::error::no rpc_register() calls found in $FIRMWARE_SRC; extraction is broken" >&2
    exit 1
fi
if [ -z "$spec_methods" ]; then
    echo "::error::no /rpc/ paths found in $SPEC; extraction is broken" >&2
    exit 1
fi

fail=0

dupes=$(printf '%s\n' "$spec_methods_all" | uniq -d)
if [ -n "$dupes" ]; then
    fail=1
    echo "::error::duplicate path keys in $SPEC (YAML keeps only the last one):" >&2
    printf '%s\n' "$dupes" | sed 's/^/  /' >&2
fi

missing=$(comm -23 <(printf '%s\n' "$fw_methods") <(printf '%s\n' "$spec_methods"))
if [ -n "$missing" ]; then
    fail=1
    echo "::error::methods registered in $FIRMWARE_SRC but missing from $SPEC:" >&2
    printf '%s\n' "$missing" | sed 's/^/  /' >&2
fi

phantom=$(comm -13 <(printf '%s\n' "$fw_methods") <(printf '%s\n' "$spec_methods"))
if [ -n "$phantom" ]; then
    fail=1
    echo "::error::methods documented in $SPEC but not registered in $FIRMWARE_SRC:" >&2
    printf '%s\n' "$phantom" | sed 's/^/  /' >&2
fi

# Webapp call-site check: every bramble.* method the webapp invokes through
# client.rpc(...) must be a method the firmware registers. The name-symmetry
# checks above only compare the firmware registry against the spec, so they
# cannot see a webapp that calls a method existing in neither (a phantom like
# bramble.disconnect, issue #101). This closes that gap.
WEBAPP_SRC="webapp/src"
if [ -d "$WEBAPP_SRC" ]; then
    # NO SKIP PATH HERE, DELIBERATELY. This check used to run under perl and
    # emit `::warning::perl not found; skipping` when perl was absent, which
    # meant a runner image that dropped the interpreter would silently stop
    # enforcing half of this gate while Static checks still reported success.
    # That is the advisory-check-in-disguise failure mode the clang-format,
    # ruff, and commitlint version asserts in firmware-quality.yml exist to
    # prevent, so it fails loud instead. python3 rather than perl because CI
    # already depends on it outright (the host, gosim, and webapp coverage
    # ratchets are all python3), so this stops being an extra dependency at
    # all rather than trading one optional interpreter for another.
    if ! command -v python3 >/dev/null 2>&1; then
        echo "::error::python3 not found, so the webapp RPC call-site check cannot run." >&2
        echo "  This check is required, not optional: it is the only thing that catches a" >&2
        echo "  webapp calling a method that exists in neither $SPEC nor $FIRMWARE_SRC." >&2
        exit 1
    fi
    # Capture the first string-literal argument of every .rpc(...) call,
    # tolerating a generic type argument and a newline before the literal.
    # Matches .rpc(, never .sendRPC( (used with a deliberately fake method
    # in transport tests), so those stay out of the comparison.
    #
    # `sort -u` on the result, not python's own sorted(), so both sides of the
    # comm below are ordered by the SAME collation. comm compares byte order
    # under the ambient locale and silently misreports when its two inputs
    # disagree about it.
    webapp_calls=$(python3 - "$WEBAPP_SRC" <<'PY' | sort -u
import os
import re
import sys

root = sys.argv[1]
pattern = re.compile(r"""\.rpc\s*(?:<[^>]*>)?\s*\(\s*(["'])(bramble\.[A-Za-z0-9_.]+)\1""")

for dirpath, _dirnames, filenames in os.walk(root):
    for filename in filenames:
        if not filename.endswith((".ts", ".tsx")):
            continue
        path = os.path.join(dirpath, filename)
        with open(path, encoding="utf-8", errors="replace") as handle:
            for match in pattern.finditer(handle.read()):
                print(match.group(2))
PY
    )
    if [ -n "$webapp_calls" ]; then
        webapp_phantom=$(comm -23 <(printf '%s\n' "$webapp_calls") <(printf '%s\n' "$fw_methods"))
        if [ -n "$webapp_phantom" ]; then
            fail=1
            echo "::error::methods the webapp calls via client.rpc() but $FIRMWARE_SRC does not register:" >&2
            printf '%s\n' "$webapp_phantom" | sed 's/^/  /' >&2
        fi
    fi
fi

# Method-table capacity check: rpc_register() silently drops methods past
# CONFIG_BRAMBLE_RPC_MAX_METHODS (boot logs an error nobody reads and the
# method is simply absent at runtime). 1.9.0 shipped with 69 registrations
# against a 64-entry table, losing getBeaconPolicy and the phy.* debug
# surface on every device. The name-symmetry checks above cannot see this
# because the spec and the registry still agree textually. Fail when the
# registration count reaches any configured cap, counting duplicate
# registrations too (each one consumes a table slot).
count_all=$(grep -c 'rpc_register("' "$FIRMWARE_SRC")
caps=$(grep -rhoE 'CONFIG_BRAMBLE_RPC_MAX_METHODS[= ][0-9]+' \
    sdkconfig.defaults sdkconfig.defaults.* nrf/shim/include/sdkconfig.h \
    emulator/node/sdkconfig.defaults emulator/node/sdkconfig.defaults.* 2>/dev/null \
    | grep -oE '[0-9]+$' | sort -n -u)
kconfig_default=$(grep -A8 'config BRAMBLE_RPC_MAX_METHODS' components/rpc/Kconfig | grep -oE 'default [0-9]+' | grep -oE '[0-9]+' | head -1)
caps=$(printf '%s\n%s\n' "$caps" "$kconfig_default" | grep -E '^[0-9]+$' | sort -n -u)
if [ -z "$caps" ]; then
    fail=1
    echo "::error::could not extract any CONFIG_BRAMBLE_RPC_MAX_METHODS value; capacity check is broken" >&2
else
    min_cap=$(printf '%s\n' "$caps" | head -1)
    if [ "$count_all" -ge "$min_cap" ]; then
        fail=1
        echo "::error::$count_all rpc_register() calls in $FIRMWARE_SRC but the smallest configured" >&2
        echo "  CONFIG_BRAMBLE_RPC_MAX_METHODS is $min_cap: registrations past the cap are silently" >&2
        echo "  dropped at boot. Raise the cap in every sdkconfig default (and the Kconfig default)." >&2
    fi
fi

count=$(printf '%s\n' "$fw_methods" | wc -l)
if [ "$fail" -ne 0 ]; then
    echo "RPC contract check FAILED: spec and firmware registry have drifted." >&2
    echo "Fix $SPEC (or the registration table) so both list the same methods." >&2
    exit 1
fi

echo "RPC contract check OK: $count methods match between $FIRMWARE_SRC and $SPEC."
