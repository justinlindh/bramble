#!/usr/bin/env bash
# Fixture tests for scripts/lib/crypt-state.sh.
#
# The parser decides whether a board has flash encryption burned before an
# image is written, and a wrong verdict bricks the board, so every acceptance
# rule is pinned here against fixture `espefuse ... summary` output with no
# hardware. Run directly:
#   bash scripts/test-crypt-state.sh
# Exit 0 if every case passes, 1 otherwise.

set -u

# shellcheck source=scripts/lib/crypt-state.sh
source "$(cd "$(dirname "$0")" && pwd)/lib/crypt-state.sh"

PASS=0
FAIL=0

# expect_state <name> <expected_output> <summary_text>
expect_state() {
  local name="$1" want="$2" summary="$3" got
  got="$(printf '%s\n' "$summary" | crypt_state_from_summary)"
  if [[ "$got" == "$want" ]]; then
    echo "PASS: $name -> '${got}'"
    PASS=$((PASS + 1))
  else
    echo "FAIL: $name (want '${want}', got '${got}')"
    FAIL=$((FAIL + 1))
  fi
}

row() {
  printf 'SPI_BOOT_CRYPT_CNT (BLOCK0)  Enables flash encryption  = %s R/W (%s)' "$1" "$1"
}

expect_state "all bits clear is plaintext" "plaintext" "$(row 0b000)"
expect_state "one bit set is encrypted" "encrypted" "$(row 0b001)"
expect_state "two bits set (re-disabled) is plaintext" "plaintext" "$(row 0b011)"
expect_state "three bits set is encrypted" "encrypted" "$(row 0b111)"

expect_state "ESP32-classic FLASH_CRYPT_CNT spelling parses" "encrypted" \
  'FLASH_CRYPT_CNT (BLOCK0)  Flash encryption counter  = 0b0000001 R/W (0b0000001)'

expect_state "indented row parses" "encrypted" "  $(row 0b001)"

expect_state "empty summary is unknown" "" ""
expect_state "summary with no CRYPT_CNT row is unknown" "" \
  'WR_DIS (BLOCK0)  Disables programming of individual eFuses  = 0x0 R/W (0x00000000)'

# Fail closed when the row exists but the parenthesized value column does not:
# a bare in-prose token must not be trusted as the verdict.
expect_state "row without a (0b...) value column is unknown" "" \
  'SPI_BOOT_CRYPT_CNT (BLOCK0)  Enables flash encryption  = 0b001 R/W'

# The fuse name mentioned inside another row (a derived field, help prose) must
# not select that row's value.
expect_state "derived WR_DIS.SPI_BOOT_CRYPT_CNT row does not match" "" \
  'WR_DIS.SPI_BOOT_CRYPT_CNT (BLOCK0)  Write-protects SPI_BOOT_CRYPT_CNT  = 0b1 R/W (0b1)'

# Only the parenthesized token counts: a 0b-shaped substring inside a wider
# token (here a hex value) must not be read as the bits.
expect_state "0b-shaped substring inside a hex token is not the value" "" \
  'SPI_BOOT_CRYPT_CNT (BLOCK0)  Enables flash encryption  = 0x0b10 R/W'

# Greedy match takes the parenthesized value column (the last (0b...) group on
# the row), not an earlier parenthesized token.
expect_state "value column wins over earlier parenthesized tokens" "encrypted" \
  'SPI_BOOT_CRYPT_CNT (0b010 legacy note)  Enables flash encryption  = 0b001 R/W (0b001)'

# First matching row wins, matching a single-chip summary where only one
# spelling exists; a second row never overrides it.
expect_state "first CRYPT_CNT row wins" "plaintext" \
"$(row 0b000)
FLASH_CRYPT_CNT (BLOCK0)  Flash encryption counter  = 0b1 R/W (0b1)"

# Realistic multi-row summary: the verdict comes from the CRYPT_CNT row alone.
expect_state "full summary fixture parses the CRYPT_CNT row" "encrypted" \
'ESP32-S3 (revision v0.2)
Config fuses:
DIS_DOWNLOAD_ICACHE (BLOCK0)  Disables Icache when SoC is in Download mode  = False R/W (0b0)
Security fuses:
SPI_BOOT_CRYPT_CNT (BLOCK0)  Enables flash encryption when 1 or 3 bits are set  = 0b111 R/W (0b111)
SECURE_BOOT_EN (BLOCK0)  Enables secure boot  = False R/W (0b0)'

# crypt_cnt_lines feeds the operator-facing diagnostic in flash.sh: it must
# echo exactly the rows the parser judged, and stay quiet (status 0, no
# pipefail trip) when none exist.
lines_got="$(printf 'A\n%s\nB\n' "$(row 0b001)" | crypt_cnt_lines)"
if [[ "$lines_got" == "$(row 0b001)" ]]; then
  echo "PASS: crypt_cnt_lines echoes the CRYPT_CNT row"
  PASS=$((PASS + 1))
else
  echo "FAIL: crypt_cnt_lines echoes the CRYPT_CNT row (got '${lines_got}')"
  FAIL=$((FAIL + 1))
fi
set -o pipefail
lines_got="$(printf 'no rows here\n' | crypt_cnt_lines)" && lines_rc=0 || lines_rc=$?
set +o pipefail
if [[ "$lines_rc" == "0" && -z "$lines_got" ]]; then
  echo "PASS: crypt_cnt_lines is empty and status 0 with no matching row"
  PASS=$((PASS + 1))
else
  echo "FAIL: crypt_cnt_lines with no matching row (rc=$lines_rc, got '${lines_got}')"
  FAIL=$((FAIL + 1))
fi

echo
echo "crypt-state tests: $PASS passed, $FAIL failed"
[[ "$FAIL" == "0" ]]
