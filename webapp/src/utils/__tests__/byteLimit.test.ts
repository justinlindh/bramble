import { describe, it, expect } from 'vitest';
import { clampToUtf8Bytes, utf8Length } from '../byteLimit';

describe('utf8Length', () => {
  it('counts ASCII as one byte each', () => {
    expect(utf8Length('')).toBe(0);
    expect(utf8Length('hello')).toBe(5);
  });

  it('counts multi-byte code points by their UTF-8 width', () => {
    expect(utf8Length('é')).toBe(2); // U+00E9
    expect(utf8Length('€')).toBe(3); // U+20AC
    expect(utf8Length('😀')).toBe(4); // U+1F600, one code point, four bytes
    expect(utf8Length('a€😀')).toBe(1 + 3 + 4);
  });
});

describe('clampToUtf8Bytes', () => {
  it('returns an empty string for a non-positive budget', () => {
    expect(clampToUtf8Bytes('anything', 0)).toBe('');
    expect(clampToUtf8Bytes('anything', -5)).toBe('');
  });

  it('returns the input unchanged when it already fits', () => {
    expect(clampToUtf8Bytes('hi', 2)).toBe('hi');
    expect(clampToUtf8Bytes('a€', 4)).toBe('a€');
  });

  it('cuts on a code-point boundary rather than mid-character', () => {
    // 'a' is one byte, '€' is three; a four-byte input clamped to three keeps
    // only the 'a' instead of half of the euro sign.
    expect(clampToUtf8Bytes('a€', 3)).toBe('a');
  });

  it('keeps or drops an astral code point whole', () => {
    // '😀' is a single four-byte code point: a three-byte budget cannot hold
    // any of it, and it is never split into an invalid surrogate.
    expect(clampToUtf8Bytes('😀', 3)).toBe('');
    expect(clampToUtf8Bytes('😀', 4)).toBe('😀');
    expect(clampToUtf8Bytes('ab😀c', 6)).toBe('ab😀');
    expect(clampToUtf8Bytes('ab😀c', 5)).toBe('ab');
  });
});
