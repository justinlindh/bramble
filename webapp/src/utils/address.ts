// Canonical formatting for numeric node addresses.
//
// A node address is a uint32 that the app carries around as a JS number
// (config.identity.address, peer addresses, message-store keys, etc.). The
// 8-char uppercase hex form is the display/key convention across the webapp.
// The same form is used for other uint32 identity values such as pubkey
// hashes. `>>> 0` normalizes to uint32 so a high-bit value (e.g. 0xDEADBEEF)
// does not stringify as a negative number.

/** 8-char uppercase hex form of a numeric node address, e.g. 0xDEADBEEF -> "DEADBEEF". */
export function formatAddrHex(addr: number): string {
  return (addr >>> 0).toString(16).toUpperCase().padStart(8, '0');
}

/** "0x"-prefixed 8-char hex form, e.g. 0xDEADBEEF -> "0xDEADBEEF". */
export function formatAddr0x(addr: number): string {
  return `0x${formatAddrHex(addr)}`;
}

/** Short "0x"-prefixed form: the low 16 bits, e.g. 0xDEADBEEF -> "0xBEEF". */
export function formatAddrShort(addr: number): string {
  return `0x${formatAddrHex(addr).slice(-4)}`;
}
