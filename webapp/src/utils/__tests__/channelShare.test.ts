import { describe, it, expect } from 'vitest';
import { encodeNodeShare, parseNodeShare, parseChannelShare } from '../channelShare';

describe('parseNodeShare', () => {
  it('round-trips an encoded node share', () => {
    const uri = encodeNodeShare('alice', 0xdeadbeef, '');
    const r = parseNodeShare(uri);
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.data.name).toBe('alice');
      expect(r.data.address).toBe(0xdeadbeef);
    }
  });

  it('accepts an all-decimal-digit address as hex, not decimal', () => {
    const r = parseNodeShare('bramble://node/v1?n=n&a=12345678');
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.data.address).toBe(0x12345678);
  });

  it('accepts the all-zero address, which is a value and not a parse failure', () => {
    const r = parseNodeShare('bramble://node/v1?n=n&a=00000000');
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.data.address).toBe(0);
  });

  it('accepts uppercase hex, which is how firmware renders addresses', () => {
    const r = parseNodeShare('bramble://node/v1?n=n&a=DEADBEEF');
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.data.address).toBe(0xdeadbeef);
  });

  it('rejects an address with trailing non-hex garbage instead of truncating it', () => {
    // parseInt('abcz', 16) is 0xabc (2748), a wrong-but-plausible address;
    // tryParseAddr rejects it outright.
    const r = parseNodeShare('bramble://node/v1?n=n&a=abcz');
    expect(r.ok).toBe(false);
  });

  it('rejects an address wider than the 32-bit range', () => {
    const r = parseNodeShare('bramble://node/v1?n=n&a=1deadbeef');
    expect(r.ok).toBe(false);
  });

  it('rejects a missing address', () => {
    expect(parseNodeShare('bramble://node/v1?n=n').ok).toBe(false);
  });

  it('rejects a non-node share string', () => {
    expect(parseNodeShare('bramble://ch/v1?n=x').ok).toBe(false);
  });
});

describe('parseChannelShare', () => {
  it('parses a channel name and PSK', () => {
    const r = parseChannelShare('bramble://ch/v1?n=general&k=abcd');
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.data.name).toBe('general');
      expect(r.data.psk).toBe('abcd');
    }
  });

  it('rejects a share with no channel name', () => {
    expect(parseChannelShare('bramble://ch/v1?k=abcd').ok).toBe(false);
  });
});
