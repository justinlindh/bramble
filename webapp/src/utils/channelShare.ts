/**
 * Channel / Node share string codec.
 *
 * Channel format:  bramble://ch/v1?n={name}&k={psk_hex}
 *   - `n`  URL-encoded channel name (required)
 *   - `k`  hex-encoded PSK (omitted when no PSK)
 *
 * Node format:  bramble://node/v1?n={name}&a={addr_hex}&pk={pubkey_base64url}
 *   - `n`   URL-encoded node name
 *   - `a`   hex address without 0x prefix
 *   - `pk`  base64url-encoded public key
 */

export interface ChannelShareData {
  name: string;
  psk?: string; // hex-encoded raw PSK bytes (optional)
}

export interface NodeShareData {
  name: string;
  address: number;
  pubkeyB64: string;
}

// ─── Encode ────────────────────────────────────────────────────────────────

export function encodeChannelShare(name: string, psk?: string): string {
  const params = new URLSearchParams();
  params.set('n', name);
  if (psk && psk.trim()) {
    params.set('k', psk.trim());
  }
  return `bramble://ch/v1?${params.toString()}`;
}

export function encodeNodeShare(
  name: string,
  address: number,
  pubkeyB64: string
): string {
  const params = new URLSearchParams();
  params.set('n', name);
  params.set('a', address.toString(16).padStart(8, '0'));
  if (pubkeyB64) {
    // Convert standard base64 → base64url for URL safety
    params.set('pk', pubkeyB64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, ''));
  }
  return `bramble://node/v1?${params.toString()}`;
}

// ─── Decode ────────────────────────────────────────────────────────────────

export type ParseResult<T> =
  | { ok: true; data: T }
  | { ok: false; error: string };

/**
 * Shared prefix-check + query decode for the ParseResult-style share parsers.
 * Fails with `notValidError` when the prefix is wrong and with a generic
 * "Malformed share string." if URLSearchParams construction throws, leaving
 * each caller to validate its own fields. Mirrors anchorShare.ts's paramsFor,
 * adapted to carry a distinct not-a-share message.
 */
export function parseShareParams(
  input: string,
  prefix: string,
  notValidError: string
): ParseResult<URLSearchParams> {
  const s = input.trim();
  if (!s.startsWith(prefix)) {
    return { ok: false, error: notValidError };
  }
  try {
    return { ok: true, data: new URLSearchParams(s.slice(prefix.length)) };
  } catch {
    return { ok: false, error: 'Malformed share string.' };
  }
}

export function parseChannelShare(input: string): ParseResult<ChannelShareData> {
  const parsed = parseShareParams(input, 'bramble://ch/v1?', 'Not a valid Bramble channel share string.');
  if (!parsed.ok) return parsed;
  const params = parsed.data;
  const name = params.get('n');
  if (!name || name.trim() === '') {
    return { ok: false, error: 'Missing channel name.' };
  }
  const psk = params.get('k') ?? undefined;
  return { ok: true, data: { name: name.trim(), psk } };
}

export function parseNodeShare(input: string): ParseResult<NodeShareData> {
  const parsed = parseShareParams(input, 'bramble://node/v1?', 'Not a valid Bramble node share string.');
  if (!parsed.ok) return parsed;
  const params = parsed.data;
  const name = params.get('n') ?? '';
  const addrHex = params.get('a');
  const pkUrl = params.get('pk');
  if (!addrHex) {
    return { ok: false, error: 'Missing node address.' };
  }
  const address = parseInt(addrHex, 16);
  if (isNaN(address)) {
    return { ok: false, error: 'Invalid node address.' };
  }
  // Convert base64url → standard base64
  const pubkeyB64 = pkUrl
    ? pkUrl.replace(/-/g, '+').replace(/_/g, '/') + '=='.slice(0, (4 - (pkUrl.length % 4)) % 4)
    : '';
  return { ok: true, data: { name, address, pubkeyB64 } };
}

/** Returns true if the string looks like any Bramble share (channel, node, or network key). */
export function isBrambleShare(s: string): boolean {
  const trimmed = s.trim();
  return (
    trimmed.startsWith('bramble://ch/v1?') ||
    trimmed.startsWith('bramble://node/v1?') ||
    trimmed.startsWith('bramble://net/v1?')
  );
}
