/**
 * Network-key share string codec.
 *   bramble://net/v1?k={key_hex}   (k = 64 hex chars, the raw 32-byte network key)
 *
 * The network key is a write-only secret: this codec exists to carry a
 * freshly-generated key out-of-band (QR / copy-paste) to each node, mirroring
 * the channel-PSK share pattern. It is never read back from a device; only the
 * one-way fingerprint (SHA256(key)[0:4]) is surfaced for verification.
 */
import { parseShareParams, type ParseResult } from './channelShare';
import { normalizeHex64 } from './hex';

const PREFIX = 'bramble://net/v1?';

export function encodeNetworkKeyShare(keyHex: string): string {
  const params = new URLSearchParams();
  params.set('k', keyHex.trim().toLowerCase());
  return `${PREFIX}${params.toString()}`;
}

export function parseNetworkKeyShare(input: string): ParseResult<{ key: string }> {
  const parsed = parseShareParams(input, PREFIX, 'Not a valid Bramble network-key share string.');
  if (!parsed.ok) return parsed;
  const key = parsed.data.get('k');
  const norm = key ? normalizeHex64(key) : null;
  if (!norm) {
    return { ok: false, error: 'Missing or malformed network key (need 64 hex chars).' };
  }
  return { ok: true, data: { key: norm } };
}
