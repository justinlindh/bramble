#!/usr/bin/env bash
# Anti-drift gate: every ARM cross-toolchain version reference in the tree must
# match the single source of truth in `.arm-gcc-version`.
#
# Why this exists: the nRF52840 target's RAM budget gate (nrf/scripts/size_report.py)
# fails the build on a byte count, and that byte count is a property of the
# compiler as much as of the source. CI builds with the version baked into the
# runner image; a developer builds with whatever their distro ships, which on a
# rolling distro is several GCC majors ahead. The two numbers were being compared
# as if they were the same measurement, so a local "8 bytes under budget" and a
# CI over-budget failure looked like a mystery instead of two different compilers.
# The pin here is what makes a local number and a CI number the same measurement.
#
# Bumping the toolchain is a deliberate act: edit `.arm-gcc-version`, bump
# ARM_GCC_VERSION in the private runner-image definition so the image agrees,
# update every file this script names, and re-measure the nRF budget margins in
# the same change. The version string lives in exactly one place, so a bump never
# means editing this script, only the pinned files it points at.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

self="scripts/lint/check-arm-gcc-version.sh"
sot_file=".arm-gcc-version"
fail=0
report() { printf 'check-arm-gcc-version: %s\n' "$1" >&2; fail=1; }

if [[ ! -f "$sot_file" ]]; then
  echo "check-arm-gcc-version: missing source of truth file $sot_file" >&2
  exit 1
fi

# The compiler's own `--version` form, `13.2.1`. Arm publishes the same compiler
# under a release label that reorders the patch field (`13.2.Rel1`), and the
# download URLs use that label, so derive it rather than storing a second copy
# that could disagree with the first.
want="$(tr -d '[:space:]' < "$sot_file")"
if [[ ! "$want" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "check-arm-gcc-version: $sot_file must hold a bare MAJOR.MINOR.PATCH gcc version, got '$want'" >&2
  exit 1
fi
major="${want%%.*}"
patch="${want##*.}"
minor="${want#*.}"; minor="${minor%%.*}"
rel="${major}.${minor}.Rel${patch}"

# 1. Required pins. Each entry is "<file>::<literal that must appear>". These are
#    the load-bearing references: the two that decide which compiler a build
#    actually checks against, plus the prose a contributor follows to install it.
#    The build and the CI assert must READ the source of truth, not restate it,
#    so what is asserted there is the read itself.
required=(
  ".github/workflows/quality.yml::want=\"\$(tr -d '[:space:]' < .arm-gcc-version)\""
  "nrf/CMakeLists.txt::${sot_file}"
  "nrf/scripts/size_report.py::${sot_file}"
  "nrf/README.md::arm-none-eabi-gcc ${want}"
  # The Ubuntu package version CI installs embeds Arm's release label
  # (`15:13.2.rel1-2`), so asserting the derived label keeps the container recipe
  # that reproduces CI's byte counts from drifting off the pin.
  "nrf/README.md::${rel}"
  "CONTRIBUTING.md::arm-none-eabi-gcc ${want}"
  "CONTRIBUTING.md::${sot_file}"
  "CLAUDE.md::arm-none-eabi-gcc ${want}"
  "docs/quality-policy.md::arm-none-eabi-gcc ${want}"
)
for entry in "${required[@]}"; do
  file="${entry%%::*}"
  literal="${entry#*::}"
  if [[ ! -f "$file" ]]; then
    report "required pin file is missing: $file"
    continue
  fi
  # Case-insensitive: Arm spells its own release label both ways, `13.2.rel1`
  # in the download path and `13.2.Rel1` in the directory the tarball unpacks to.
  if ! grep -qiF -- "$literal" "$file"; then
    report "$file no longer pins the ARM toolchain at ${want} (expected to find: ${literal})"
  fi
done

# 2. Tree-wide sweep. Any line that names the ARM cross-toolchain and carries a
#    version token on the pinned major line must name the pinned version exactly,
#    in either the gcc form or Arm's release form. Scoped to major ${major} so
#    unrelated versions on the same line (CMake 3.24, nrfx 3.9.0) stay quiet.
#    Any `N.N.RelN` token is unambiguously an Arm toolchain release, so those are
#    checked whatever their major.
hits="$(git grep -nIiE "(arm-none-eabi|gcc-arm-none-eabi|arm-gnu-toolchain|arm gnu toolchain|ARM_GCC_VERSION)" \
          -- . ":!${self}" ':!**/node_modules/**' \
        | grep -iE "((^|[^0-9.])${major}\.[0-9]+(\.[0-9]+)?([^0-9.]|$)|[0-9]+\.[0-9]+\.Rel[0-9]+)" \
        | grep -viE "((^|[^0-9.])${want//./\\.}([^0-9.]|$)|(^|[^0-9.])${rel//./\\.}([^0-9.]|$))" || true)"
if [[ -n "$hits" ]]; then
  report "ARM toolchain version reference does not match ${sot_file} (${want} / ${rel}):"
  printf '%s\n' "$hits" >&2
fi

if (( fail )); then
  echo "check-arm-gcc-version: FAIL. Source of truth is ${sot_file} (${want}, Arm release ${rel})." >&2
  exit 1
fi

echo "check-arm-gcc-version: OK (all ARM toolchain references pinned to ${want}, Arm release ${rel})"
