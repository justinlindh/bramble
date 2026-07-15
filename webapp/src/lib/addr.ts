// Canonical parsing for numeric node addresses coming off the wire.
//
// Firmware sends addresses as 8-char hex strings (e.g. "F2BE6EEE"); some
// call sites already carry an in-app numeric address; a field can also be
// entirely absent on a partial/legacy payload. This consolidates the
// `typeof x === 'string' ? parseInt(x, 16) : (x ?? 0)` pattern that used to
// be hand-rolled at each incoming-message/ack normalization site.
//
// The formatting counterpart (`formatAddrHex`) already lives in
// `../utils/address.ts`, added for #168. Deliberately not duplicated here.

/** Numeric node address from a hex string, a passthrough number, or 0 if absent. */
export function parseAddr(x: string | number | undefined): number {
  if (typeof x === 'string') return parseInt(x, 16);
  if (typeof x === 'number') return x;
  return 0;
}
