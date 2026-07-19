#!/usr/bin/env bash
# Host-side dry-run tests for scripts/antirollback-guard.sh (issue #79).
#
# Exercises the consent-gate logic against fixture sdkconfigs with NO hardware,
# no IDF, and no flashing: the guard is a pure function of the config file,
# its flags, and stdin. Run directly:
#   bash scripts/test-antirollback-guard.sh
# Exit 0 if every case passes, 1 otherwise.

set -u

GUARD="$(cd "$(dirname "$0")" && pwd)/antirollback-guard.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

# expect <name> <expected_exit> <stdin_text|-> guard-args...
expect() {
  local name="$1" want="$2" stdin="$3"
  shift 3
  local got out
  if [[ "$stdin" == "-" ]]; then
    out="$(bash "$GUARD" "$@" </dev/null 2>&1)"
  else
    out="$(printf '%s\n' "$stdin" | bash "$GUARD" "$@" 2>&1)"
  fi
  got=$?
  if [[ "$got" == "$want" ]]; then
    echo "PASS: $name (exit $got)"
    PASS=$((PASS + 1))
  else
    echo "FAIL: $name (want exit $want, got $got)"
    while IFS= read -r line; do printf '    | %s\n' "$line"; done <<<"$out"
    FAIL=$((FAIL + 1))
  fi
}

# expect_output <name> <grep_pattern> <stdin_text|-> guard-args...
expect_output() {
  local name="$1" pattern="$2" stdin="$3"
  shift 3
  local out
  if [[ "$stdin" == "-" ]]; then
    out="$(bash "$GUARD" "$@" </dev/null 2>&1 || true)"
  else
    out="$(printf '%s\n' "$stdin" | bash "$GUARD" "$@" 2>&1 || true)"
  fi
  if grep -q "$pattern" <<<"$out"; then
    echo "PASS: $name (output contains '$pattern')"
    PASS=$((PASS + 1))
  else
    echo "FAIL: $name (output missing '$pattern')"
    while IFS= read -r line; do printf '    | %s\n' "$line"; done <<<"$out"
    FAIL=$((FAIL + 1))
  fi
}

# Fixtures
PLAIN="$TMP/sdkconfig.plain"
cat >"$PLAIN" <<'EOF'
CONFIG_IDF_TARGET="esp32s3"
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
EOF

AR3="$TMP/sdkconfig.ar3"
cat >"$AR3" <<'EOF'
CONFIG_IDF_TARGET="esp32s3"
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y
CONFIG_BOOTLOADER_APP_SECURE_VERSION=3
EOF

AR_EMU="$TMP/sdkconfig.aremu"
cat >"$AR_EMU" <<'EOF'
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y
CONFIG_BOOTLOADER_APP_SECURE_VERSION=1
CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE=y
EOF

# 1. Default flow is untouched: plain build flashes with exit 0 and no banner.
expect "plain config, flash, no flag: silent pass" 0 - \
  --sdkconfig "$PLAIN" --action flash
out="$(bash "$GUARD" --sdkconfig "$PLAIN" --action flash </dev/null 2>&1)"
if [[ -z "$out" ]]; then
  echo "PASS: plain config produces no output"
  PASS=$((PASS + 1))
else
  echo "FAIL: plain config produced output: $out"
  FAIL=$((FAIL + 1))
fi

# 2. Anti-rollback build action: allowed but loud.
expect "AR config, build action: allowed" 0 - \
  --sdkconfig "$AR3" --action build
expect_output "AR config, build action: loud banner" "ANTI-ROLLBACK BUILD" - \
  --sdkconfig "$AR3" --action build
expect_output "AR config, build action: names the epoch" "= 3" - \
  --sdkconfig "$AR3" --action build

# 3. Flash without the flag: refused.
expect "AR config, flash, no flag: refused" 2 - \
  --sdkconfig "$AR3" --action flash
expect_output "refusal names the required flag" "enable-antirollback" - \
  --sdkconfig "$AR3" --action flash

# 4. Flash with flag but wrong phrase: refused.
expect "AR config, flash, flag, wrong phrase: refused" 2 "yes" \
  --sdkconfig "$AR3" --action flash --enable-antirollback
expect "AR config, flash, flag, wrong epoch in phrase: refused" 2 "BURN EPOCH 2" \
  --sdkconfig "$AR3" --action flash --enable-antirollback

# 5. Flash with flag and EOF on stdin (non-interactive): refused.
expect "AR config, flash, flag, EOF stdin: refused" 2 - \
  --sdkconfig "$AR3" --action flash --enable-antirollback

# 6. Flash with flag and the exact epoch phrase: allowed.
expect "AR config, flash, flag, correct phrase: allowed" 0 "BURN EPOCH 3" \
  --sdkconfig "$AR3" --action flash --enable-antirollback
expect_output "confirmation flow states irreversibility" "IRREVERSIBL" "BURN EPOCH 3" \
  --sdkconfig "$AR3" --action flash --enable-antirollback
expect_output "device floor shown as UNKNOWN without a port" "UNKNOWN" "BURN EPOCH 3" \
  --sdkconfig "$AR3" --action flash --enable-antirollback

# 7. Emulated mode: same ceremony, clearly labeled as reversible emulation.
expect "emulated AR, flash, flag, correct phrase: allowed" 0 "BURN EPOCH 1" \
  --sdkconfig "$AR_EMU" --action flash --enable-antirollback
expect_output "emulated mode labeled" "EMULATED" "BURN EPOCH 1" \
  --sdkconfig "$AR_EMU" --action flash --enable-antirollback

# 8. Missing sdkconfig: fail closed.
expect "missing sdkconfig: fail closed" 2 - \
  --sdkconfig "$TMP/nope" --action flash

# 9. Usage errors.
expect "bad action: refused" 2 - --sdkconfig "$PLAIN" --action fry
expect "unknown flag: refused" 2 - --sdkconfig "$PLAIN" --action flash --frobnicate

echo ""
echo "antirollback-guard tests: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
