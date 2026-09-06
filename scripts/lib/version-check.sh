# shellcheck shell=bash
#
# Shared harness for the anti-drift version gates: check-idf-version.sh,
# check-node-version.sh, and check-arm-gcc-version.sh. Each gate reads its
# version from a single source-of-truth file (.esp-idf-version, .nvmrc,
# .arm-gcc-version) and asserts that every reference in the tree matches it.
#
# The parts factored out here are the ones that were byte-identical across all
# three gates: reading the source-of-truth file, the diagnostic wording, and
# the required-pins loop. The pins themselves, the version-format validation,
# the tree-wide sweeps, and the final OK/FAIL wording stay in each gate, because
# those are the per-tool data and messages a reader goes to that gate to see.
# This keeps the loop logic in one audited place without hiding what each gate
# actually checks.
#
# These helpers are stateless: each takes the gate's diagnostic name as its
# first argument and reports failures via its return status or stderr, so no
# gate has to share a mutable flag variable with this file.

# vc_read_sot <name> <file>: echo the whitespace-stripped source-of-truth value,
# or exit 1 (not just fail) if the file is missing, since a gate cannot check
# anything without it.
vc_read_sot() {
  local name="$1" file="$2"
  if [[ ! -f "$file" ]]; then
    echo "$name: missing source of truth file $file" >&2
    exit 1
  fi
  tr -d '[:space:]' < "$file"
}

# vc_report <name> <message>: print a namespaced diagnostic to stderr. The
# caller raises its own failure flag; this only formats the line.
vc_report() {
  printf '%s: %s\n' "$1" "$2" >&2
}

# vc_require_pins <name> <subject> [-i] <entry>...: each entry is
# "<file>::<literal>". Print a diagnostic for any entry whose file is missing or
# does not contain the literal, and return 1 if any entry failed (0 otherwise).
# <subject> is the human phrase naming what the pin fixes (e.g. "ESP-IDF
# v5.4.1"); -i makes the literal match case-insensitive, which the ARM gate
# needs because Arm spells its release label both Rel1 and rel1.
vc_require_pins() {
  local name="$1" subject="$2"; shift 2
  local grep_flags=(-qF)
  if [[ "${1:-}" == "-i" ]]; then
    grep_flags=(-qiF)
    shift
  fi
  local entry file literal rc=0
  for entry in "$@"; do
    file="${entry%%::*}"
    literal="${entry#*::}"
    if [[ ! -f "$file" ]]; then
      vc_report "$name" "required pin file is missing: $file"
      rc=1
      continue
    fi
    if ! grep "${grep_flags[@]}" -- "$literal" "$file"; then
      vc_report "$name" "$file no longer pins ${subject} (expected to find: ${literal})"
      rc=1
    fi
  done
  return "$rc"
}
