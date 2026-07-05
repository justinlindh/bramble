/**
 * Network-key share string codec.
 *   bramble://net/v1?k={key_hex}   (k = 64 hex chars, the raw 32-byte network key)
 *
 * The network key is a write-only secret: this codec exists to carry a
 * freshly-generated key out-of-band (QR / copy-paste) to each node, mirroring
 * the channel-PSK share pattern. It is never read back from a device; only the
 * one-way fingerprint (SHA256(key)[0:4]) is surfaced for verification.
 */
import type { ParseResult } from './channelShare';

const PREFIX = 'bramble://net/v1?';
const HEX64 = /^[0-9a-fA-F]{64}$/;

export function encodeNetworkKeyShare(keyHex: string): string {
  const params = new URLSearchParams();
  params.set('k', keyHex.trim().toLowerCase());
  return `${PREFIX}${params.toString()}`;
}

export function parseNetworkKeyShare(input: string): ParseResult<{ key: string }> {
  const s = input.trim();
  if (!s.startsWith(PREFIX)) {
    return { ok: false, error: 'Not a valid Bramble network-key share string.' };
  }
  try {
    const params = new URLSearchParams(s.slice(PREFIX.length));
    const key = params.get('k');
    if (!key || !HEX64.test(key.trim())) {
      return { ok: false, error: 'Missing or malformed network key (need 64 hex chars).' };
    }
    return { ok: true, data: { key: key.trim().toLowerCase() } };
  } catch {
    return { ok: false, error: 'Malformed share string.' };
  }
}
