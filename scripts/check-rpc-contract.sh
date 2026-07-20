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
    if ! command -v perl >/dev/null 2>&1; then
        echo "::warning::perl not found; skipping webapp RPC call-site check" >&2
    else
        # Capture the first string-literal argument of every .rpc(...) call,
        # tolerating a generic type argument and a newline before the literal.
        # Matches .rpc(, never .sendRPC( (used with a deliberately fake method
        # in transport tests), so those stay out of the comparison.
        webapp_calls=$(find "$WEBAPP_SRC" \( -name '*.ts' -o -name '*.tsx' \) -print0 \
            | xargs -0 perl -0777 -ne \
                'while(/\.rpc\s*(?:<[^>]*>)?\s*\(\s*(["\x27])(bramble\.[A-Za-z0-9_.]+)\1/g){print "$2\n"}' \
            | sort -u)
        if [ -n "$webapp_calls" ]; then
            webapp_phantom=$(comm -23 <(printf '%s\n' "$webapp_calls") <(printf '%s\n' "$fw_methods"))
            if [ -n "$webapp_phantom" ]; then
                fail=1
                echo "::error::methods the webapp calls via client.rpc() but $FIRMWARE_SRC does not register:" >&2
                printf '%s\n' "$webapp_phantom" | sed 's/^/  /' >&2
            fi
        fi
    fi
fi

count=$(printf '%s\n' "$fw_methods" | wc -l)
if [ "$fail" -ne 0 ]; then
    echo "RPC contract check FAILED: spec and firmware registry have drifted." >&2
    echo "Fix $SPEC (or the registration table) so both list the same methods." >&2
    exit 1
fi

echo "RPC contract check OK: $count methods match between $FIRMWARE_SRC and $SPEC."
