// Canonical parsing for numeric node addresses coming off the wire.
//
// Firmware sends addresses as 8-char hex strings (e.g. "DEADBEEF"); some
// call sites already carry an in-app numeric address; a field can also be
// entirely absent on a partial/legacy payload. This is the single parser for
// that shape, consolidating the hand-rolled variants that used to live at each
// normalization site (an optional 0x prefix, whitespace, and NaN all had to be
// re-handled per copy, and one copy wrongly treated an all-decimal-digit hex
// address as decimal).
//
// The formatting counterpart (`formatAddrHex`) already lives in
// `../utils/address.ts`, added for #168. Deliberately not duplicated here.

/**
 * Destination address that reaches every node: a message with `to ===
 * BROADCAST_ADDR` is a broadcast. This is load-bearing protocol semantics, so
 * it lives here once rather than as a bare `0xffffffff` literal repeated across
 * the store, the messaging pipeline, and the chat surfaces.
 */
export const BROADCAST_ADDR = 0xffffffff;

/**
 * Internal sentinel for a channel conversation's destination. It is not sent on
 * the wire: the send path folds it to `BROADCAST_ADDR` and routes it through
 * `sendMessage` with a channel index (unlike a plain broadcast, which has no
 * channel and goes via `sendBroadcast`). One character away from
 * `BROADCAST_ADDR`, which is exactly why it should never be typed by hand.
 */
export const CHANNEL_BROADCAST_ADDR = 0xfffffffe;

/**
 * Numeric node address from a hex string (with or without a `0x` prefix), a
 * passthrough number, or 0 when the input is absent, empty, or unparseable.
 * Strings are always hex, matching the firmware wire format: `"12345678"` is
 * `0x12345678`, never decimal.
 */
export function parseAddr(x: string | number | undefined): number {
  if (typeof x === 'number') return x;
  if (typeof x !== 'string') return 0;
  const raw = x.trim().replace(/^0x/i, '');
  if (!raw) return 0;
  const n = parseInt(raw, 16);
  return Number.isFinite(n) ? n : 0;
}

/**
 * Strict, validating variant for call sites that must REJECT bad input rather
 * than fall back to 0: a form field the user types into, or an imported record
 * whose keys may be malformed. Returns the numeric address from a hex string
 * (with or without a `0x` prefix, surrounding whitespace tolerated), or null
 * unless the input is exactly 1 to 8 hex digits. The 8-digit cap is the 32-bit
 * address range, and rejecting any non-hex character stops trailing garbage
 * ("abcz") from silently parsing to a truncated address, which the looser
 * parseInt path would accept. parseAddr's return-0 fallback cannot distinguish
 * "absent/invalid" from the legitimate address 0, which is why these callers
 * need their own parser.
 */
export function tryParseAddr(x: string): number | null {
  const raw = x.trim().replace(/^0x/i, '');
  if (!/^[0-9a-fA-F]{1,8}$/.test(raw)) return null;
  return parseInt(raw, 16);
}
