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

function hexToBytes(hex: string): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

/** SHA256(key)[0:4] as 8 lowercase hex chars, matching network_key_fingerprint on device. */
export async function networkKeyFingerprint(keyHex: string): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', hexToBytes(keyHex)));
  return Array.from(digest.slice(0, 4))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

/** Generate a fresh random 32-byte network key as 64 lowercase hex chars. */
export function generateNetworkKeyHex(): string {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}
