// Canonical formatting for numeric node addresses.
//
// A node address is a uint32 that the app carries around as a JS number
// (config.identity.address, peer addresses, message-store keys, etc.). The
// 8-char uppercase hex form is the display/key convention across the webapp.
// `>>> 0` normalizes to uint32 so a high-bit address (e.g. 0xF2BE6EEE) does
// not stringify as a negative number.

/** 8-char uppercase hex form of a numeric node address, e.g. 0xF2BE6EEE -> "F2BE6EEE". */
export function formatAddrHex(addr: number): string {
  return (addr >>> 0).toString(16).toUpperCase().padStart(8, '0');
}
