import { describe, it, expect } from 'vitest';
import {
  encodeChannelShare,
  parseChannelShare,
  encodeNodeShare,
  parseNodeShare,
  isBrambleShare,
} from '../../src/utils/channelShare';

// ─── Channel encode / decode ───────────────────────────────────────────────

describe('encodeChannelShare', () => {
  it('encodes a channel name without PSK', () => {
    const s = encodeChannelShare('general');
    expect(s).toBe('bramble://ch/v1?n=general');
  });

  it('encodes a channel name with PSK', () => {
    const s = encodeChannelShare('private', 'deadbeef');
    expect(s).toBe('bramble://ch/v1?n=private&k=deadbeef');
  });

  it('URL-encodes special characters in name', () => {
    const s = encodeChannelShare('my channel!');
    expect(s).toContain('n=my+channel');
  });
});

describe('parseChannelShare', () => {
  it('parses a channel without PSK', () => {
    const result = parseChannelShare('bramble://ch/v1?n=general');
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.data.name).toBe('general');
    expect(result.data.psk).toBeUndefined();
  });

  it('parses a channel with PSK', () => {
    const result = parseChannelShare('bramble://ch/v1?n=private&k=deadbeef');
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.data.name).toBe('private');
    expect(result.data.psk).toBe('deadbeef');
  });

  it('round-trips name + psk', () => {
    const encoded = encodeChannelShare('team', 'abc123def456');
    const result = parseChannelShare(encoded);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.data.name).toBe('team');
    expect(result.data.psk).toBe('abc123def456');
  });

  it('rejects non-bramble strings', () => {
    const result = parseChannelShare('https://example.com');
    expect(result.ok).toBe(false);
  });

  it('rejects strings with missing name', () => {
    const result = parseChannelShare('bramble://ch/v1?k=deadbeef');
    expect(result.ok).toBe(false);
  });

  it('trims whitespace around the input', () => {
    const result = parseChannelShare('  bramble://ch/v1?n=trimmed  ');
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.data.name).toBe('trimmed');
  });
});

// ─── Node encode / decode ──────────────────────────────────────────────────

describe('encodeNodeShare / parseNodeShare', () => {
  const sampleAddr = 0xdeadbeef;
  const samplePubkey = 'abc+def/xyz=='; // standard base64

  it('encodes a node identity', () => {
    const s = encodeNodeShare('Alice', sampleAddr, samplePubkey);
    expect(s).toContain('bramble://node/v1?');
    expect(s).toContain('n=Alice');
    expect(s).toContain('a=deadbeef');
  });

  it('round-trips address', () => {
    const s = encodeNodeShare('Bob', sampleAddr, '');
    const result = parseNodeShare(s);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.data.address).toBe(sampleAddr);
    expect(result.data.name).toBe('Bob');
  });

  it('round-trips pubkey (base64 ↔ base64url)', () => {
    const s = encodeNodeShare('Carol', 0x12345678, samplePubkey);
    const result = parseNodeShare(s);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    // Should recover the standard base64 characters (+ and /)
    expect(result.data.pubkeyB64).toContain('+');
    expect(result.data.pubkeyB64).toContain('/');
  });

  it('rejects non-bramble node strings', () => {
    const result = parseNodeShare('bramble://ch/v1?n=test');
    expect(result.ok).toBe(false);
  });

  it('rejects missing address', () => {
    const result = parseNodeShare('bramble://node/v1?n=test');
    expect(result.ok).toBe(false);
  });
});

// ─── isBrambleShare ────────────────────────────────────────────────────────

describe('isBrambleShare', () => {
  it('returns true for channel share', () => {
    expect(isBrambleShare('bramble://ch/v1?n=test')).toBe(true);
  });
  it('returns true for node share', () => {
    expect(isBrambleShare('bramble://node/v1?n=test&a=deadbeef')).toBe(true);
  });
  it('returns false for random string', () => {
    expect(isBrambleShare('hello world')).toBe(false);
  });
  it('handles whitespace', () => {
    expect(isBrambleShare('  bramble://ch/v1?n=test  ')).toBe(true);
  });
});
