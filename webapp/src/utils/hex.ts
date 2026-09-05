// Shared 64-hex (32-byte) key/seed handling.
//
// Anchor seeds, Ed25519 public keys, and the control-plane network key are all
// carried on the wire as exactly 64 hex characters. The anchor and network-key
// flows repeatedly need to decide "is this a bare key/seed?" and, if so, keep a
// canonical lowercase copy. Defining that policy once keeps the accepted form
// from drifting between the code paths that validate it.

/** Matches exactly 64 hex characters (a 32-byte key/seed as hex). */
export const HEX64 = /^[0-9a-fA-F]{64}$/;

/**
 * Normalize a candidate 64-hex string: trim it, and if it is exactly 64 hex
 * characters return it lowercased, otherwise return null.
 */
export function normalizeHex64(input: string): string | null {
  const s = input.trim();
  return HEX64.test(s) ? s.toLowerCase() : null;
}
