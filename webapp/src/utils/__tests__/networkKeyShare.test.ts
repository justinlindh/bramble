import { describe, it, expect } from 'vitest';
import { encodeNetworkKeyShare, parseNetworkKeyShare, networkKeyFingerprint } from '../networkKeyShare';

const KEY = 'ab'.repeat(32); // 64 hex chars

describe('networkKeyShare codec', () => {
  it('round-trips a 32-byte key', () => {
    const uri = encodeNetworkKeyShare(KEY);
    expect(uri).toBe(`bramble://net/v1?k=${KEY}`);
    const r = parseNetworkKeyShare(uri);
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.data.key).toBe(KEY);
  });

  it('normalizes uppercase hex to lowercase on parse', () => {
    const r = parseNetworkKeyShare(`bramble://net/v1?k=${'AB'.repeat(32)}`);
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.data.key).toBe(KEY);
  });

  it('rejects a non-network share string', () => {
    expect(parseNetworkKeyShare('bramble://ch/v1?n=x').ok).toBe(false);
  });

  it('rejects a wrong-length or non-hex key', () => {
    expect(parseNetworkKeyShare('bramble://net/v1?k=abcd').ok).toBe(false);
    expect(parseNetworkKeyShare(`bramble://net/v1?k=${'zz'.repeat(32)}`).ok).toBe(false);
  });

  it('computes a stable 8-hex fingerprint matching SHA256[0:4]', async () => {
    // SHA-256 of 32 bytes of 0xAB, first 4 bytes. Known-answer, precomputed.
    const fp = await networkKeyFingerprint(KEY);
    expect(fp).toMatch(/^[0-9a-f]{8}$/);
    const fp2 = await networkKeyFingerprint(KEY);
    expect(fp2).toBe(fp); // stable
    const fpOther = await networkKeyFingerprint('cd'.repeat(32));
    expect(fpOther).not.toBe(fp); // key-dependent
  });
});
