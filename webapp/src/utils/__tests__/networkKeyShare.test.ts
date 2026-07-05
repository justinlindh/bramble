import { describe, it, expect } from 'vitest';
import { encodeNetworkKeyShare, parseNetworkKeyShare } from '../networkKeyShare';

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
});
