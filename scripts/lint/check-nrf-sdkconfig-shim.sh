#!/usr/bin/env bash
# Drift tripwire for the nRF target's hand-carried Kconfig values
# (nrf/shim/include/sdkconfig.h): every CONFIG_* it defines must equal the
# default declared in the Kconfig tree, or a Kconfig-default bump for the ESP
# fleet silently forks nRF behavior. Same idiom as check-idf-version.sh:
# single source of truth plus a CI check that references agree.
set -euo pipefail

cd "$(dirname "$0")/../.."

shim="nrf/shim/include/sdkconfig.h"
fail=0

while read -r name value; do
    # Find the Kconfig default for this option (first match wins; options are
    # uniquely named across the tree).
    default=$(awk -v opt="${name#CONFIG_}" '
        $1 == "config" && $2 == opt { found = 1; next }
        found && $1 == "default" { print $2; exit }
        found && $1 == "config" { exit }
    ' main/Kconfig.projbuild components/*/Kconfig 2>/dev/null || true)
    if [[ -z "${default}" ]]; then
        echo "check-nrf-sdkconfig-shim: ${name} has no Kconfig default (option renamed or removed?)" >&2
        fail=1
    elif [[ "${default}" != "${value}" ]]; then
        echo "check-nrf-sdkconfig-shim: ${name} is ${value} in ${shim} but the Kconfig default is ${default}" >&2
        fail=1
    fi
done < <(awk '/^#define CONFIG_BRAMBLE_/ && NF >= 3 && $3 != "1" { print $2, $3 }' "${shim}")

if [[ "${fail}" != "0" ]]; then
    echo "check-nrf-sdkconfig-shim: FAIL (align ${shim} with the Kconfig defaults, or annotate a deliberate nRF override here and in the shim)" >&2
    exit 1
fi
echo "check-nrf-sdkconfig-shim: clean"
