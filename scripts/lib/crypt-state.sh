# shellcheck shell=bash
#
# Shared flash-encryption eFuse parser for the bench flash paths.
#
# scripts/flash.sh and scripts/flash-fleet.sh must answer one question before
# writing an image: does this chip have flash encryption burned? A wrong answer
# bricks the board (a plaintext image on an encrypted chip decrypts to noise,
# and the reverse is just as unbootable), so both scripts read the verdict from
# this one parser instead of hand-rolling their own. scripts/test-crypt-state.sh
# pins the behavior against fixture summaries.

# crypt_cnt_lines: echo the CRYPT_CNT eFuse row(s) from an `espefuse ...
# summary` on stdin, for diagnostics that must show the operator the same rows
# the parser judged. SPI_BOOT_CRYPT_CNT is the ESP32-S3 spelling every bramble
# board carries; FLASH_CRYPT_CNT is the ESP32-classic name, matched so the
# parser stays correct on chips bramble does not ship without changing the
# verdict on the ones it does. Anchored to the row's own name column so a
# mention of the fuse inside another row's prose or a derived field name
# (WR_DIS.SPI_BOOT_CRYPT_CNT) cannot match. Echoes nothing (status 0) when no
# row is present.
crypt_cnt_lines() {
  grep -E '^[[:space:]]*(SPI_BOOT_CRYPT_CNT|FLASH_CRYPT_CNT)[[:space:]]' || true
}

# crypt_state_from_summary: read an `espefuse ... summary` on stdin and echo the
# flash-encryption state it proves: "encrypted", "plaintext", or "" (empty) when
# no CRYPT_CNT row carries a parseable value. Callers must treat an empty result
# as "unknown" and fail closed on it.
#
# Encryption is active when an ODD number of CRYPT_CNT bits is set (0b001 and
# 0b111 enable it; 0b011 does not): the eFuse's own definition, stated on that
# same summary row. Only the parenthesized (0b...) token is accepted, the column
# espefuse prints the raw value in, so neither a reworded prose column nor a
# stray 0b-shaped substring elsewhere on the row can flip the verdict.
crypt_state_from_summary() {
  local bits ones
  bits=$(crypt_cnt_lines | sed -n 's/.*(\(0b[01]\+\)).*/\1/p' | head -1 || true)
  [[ -n "$bits" ]] || return 0
  ones=${bits//[^1]/}
  if (( ${#ones} % 2 == 1 )); then echo "encrypted"; else echo "plaintext"; fi
}
