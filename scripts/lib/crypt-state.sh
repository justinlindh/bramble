# shellcheck shell=bash
#
# Shared flash-encryption eFuse parser for the bench flash paths.
#
# Both scripts/flash.sh and scripts/flash-fleet.sh must answer one question
# before writing an image: does this chip have flash encryption burned? Getting
# it wrong bricks the board (a plaintext image on an encrypted chip decrypts to
# noise, and the reverse is just as unbootable), and it has plaintext-bricked an
# encrypted bench node once already. That decision keys off SPI_BOOT_CRYPT_CNT
# in an `espefuse ... summary`, and the odd-parity rule plus the line it reads
# lived in two hand-rolled copies that had already drifted (different grep
# targets, different bit extraction, different parity expression). One shared
# parser keeps the two paths from diverging further.

# crypt_state_from_summary: read an `espefuse ... summary` on stdin and echo the
# flash-encryption state it proves: "encrypted", "plaintext", or "" (empty) when
# no CRYPT_CNT bit pattern can be found (an unreadable or unexpected summary).
#
# Encryption is active when an ODD number of CRYPT_CNT bits is set (0b001 and
# 0b111 enable it; 0b011 does not): the eFuse's own definition, stated on that
# same summary line. SPI_BOOT_CRYPT_CNT is the ESP32-S3 spelling every bramble
# board carries; FLASH_CRYPT_CNT is the ESP32-classic name, matched too so the
# parser stays correct on chips bramble does not ship without changing the
# verdict on the ones it does. The bit pattern is read as the value itself
# rather than the prose beside it, so a wording change in espefuse output cannot
# silently flip the answer; callers treat an empty result as "unknown" and must
# fail closed on it.
crypt_state_from_summary() {
  local bits ones
  bits=$(grep -E "SPI_BOOT_CRYPT_CNT|FLASH_CRYPT_CNT" | grep -oE "0b[01]+" | tail -1 || true)
  [[ -n "$bits" ]] || return 0
  ones=${bits//[^1]/}
  if (( ${#ones} % 2 == 1 )); then echo "encrypted"; else echo "plaintext"; fi
}
