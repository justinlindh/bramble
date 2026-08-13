import { describe, it, expect } from 'vitest';
import {
  utf8Length,
  clampToUtf8Bytes,
  NODE_NAME_MAX_BYTES,
  CHANNEL_NAME_BUDGET_BYTES,
} from '../../src/utils/byteLimit';

describe('utf8Length', () => {
  it('counts bytes, not characters or UTF-16 units', () => {
    expect(utf8Length('abc')).toBe(3);
    expect(utf8Length('é')).toBe(2); // 1 char, 1 UTF-16 unit, 2 bytes
    expect(utf8Length('日')).toBe(3);
    expect(utf8Length('🌲')).toBe(4); // 1 char, 2 UTF-16 units, 4 bytes
  });

  it('disagrees with String.length exactly where the node rejects the request', () => {
    // 32 emoji pass an input capped at 32 "characters" by String.length, and
    // arrive as 128 bytes, which bramble.setNodeName refuses with -32602.
    const name = '🌲'.repeat(32);
    expect(name.length).toBe(64);
    expect(Array.from(name).length).toBe(32);
    expect(utf8Length(name)).toBe(128);
    expect(utf8Length(name)).toBeGreaterThan(NODE_NAME_MAX_BYTES);
  });
});

describe('clampToUtf8Bytes', () => {
  it('leaves a string that already fits untouched', () => {
    expect(clampToUtf8Bytes('hello', 32)).toBe('hello');
    expect(clampToUtf8Bytes('', 32)).toBe('');
  });

  it('clamps ASCII to the byte count', () => {
    expect(clampToUtf8Bytes('abcdefghij', 4)).toBe('abcd');
  });

  it('never returns more bytes than the budget', () => {
    for (const s of ['🌲🌲🌲🌲', 'ééééééé', '日本語テキスト', 'aé日🌲']) {
      for (let budget = 0; budget <= 20; budget++) {
        expect(utf8Length(clampToUtf8Bytes(s, budget))).toBeLessThanOrEqual(budget);
      }
    }
  });

  it('cuts on a character boundary rather than mid-sequence', () => {
    // A 4-byte emoji does not fit in 3 bytes, and half of one is not a
    // character: the result must drop it whole, never emit a partial
    // sequence that renders as a replacement character.
    expect(clampToUtf8Bytes('🌲', 3)).toBe('');
    expect(clampToUtf8Bytes('a🌲', 4)).toBe('a');
    expect(clampToUtf8Bytes('a🌲', 5)).toBe('a🌲');
    expect(clampToUtf8Bytes('é', 1)).toBe('');

    for (const s of ['🌲🌲', 'aé日🌲']) {
      for (let budget = 0; budget <= 12; budget++) {
        const out = clampToUtf8Bytes(s, budget);
        expect(out).not.toContain('�');
        // Round-tripping through UTF-8 is lossless only if no sequence split.
        expect(new TextDecoder().decode(new TextEncoder().encode(out))).toBe(out);
      }
    }
  });

  it('keeps the longest prefix that fits', () => {
    // Not just "safe": it should not throw away room it could have used.
    const s = 'aaé🌲';
    expect(clampToUtf8Bytes(s, 8)).toBe('aaé🌲');
    expect(clampToUtf8Bytes(s, 7)).toBe('aaé');
    expect(clampToUtf8Bytes(s, 4)).toBe('aaé');
    expect(clampToUtf8Bytes(s, 3)).toBe('aa');
  });

  it('treats a zero or negative budget as no room', () => {
    expect(clampToUtf8Bytes('abc', 0)).toBe('');
    expect(clampToUtf8Bytes('abc', -1)).toBe('');
  });

  it('holds a channel name inside the budget the node will accept', () => {
    // Each of these is 3 bytes, so five fit in the 16-byte budget and a
    // sixth would need 18.
    const clamped = clampToUtf8Bytes('日本語チャンネル名前テスト', CHANNEL_NAME_BUDGET_BYTES);
    expect(utf8Length(clamped)).toBe(15);
    expect(clamped).toBe('日本語チャ');
  });
});
