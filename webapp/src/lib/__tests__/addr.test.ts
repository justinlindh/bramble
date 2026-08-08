import { describe, it, expect } from 'vitest';
import { parseAddr, tryParseAddr } from '../addr';

describe('parseAddr', () => {
  it('passes numbers through unchanged', () => {
    expect(parseAddr(0xdeadbeef)).toBe(0xdeadbeef);
    expect(parseAddr(0)).toBe(0);
  });

  it('parses hex strings, with or without a 0x prefix and whitespace', () => {
    expect(parseAddr('DEADBEEF')).toBe(0xdeadbeef);
    expect(parseAddr('0xdeadbeef')).toBe(0xdeadbeef);
    expect(parseAddr('  0X12345678  ')).toBe(0x12345678);
  });

  it('treats all-decimal-digit strings as hex, not decimal', () => {
    expect(parseAddr('12345678')).toBe(0x12345678);
  });

  it('returns 0 for absent, empty, or unparseable input', () => {
    expect(parseAddr(undefined)).toBe(0);
    expect(parseAddr('')).toBe(0);
    expect(parseAddr('   ')).toBe(0);
    expect(parseAddr('zzz')).toBe(0);
  });
});

describe('tryParseAddr', () => {
  it('parses valid hex addresses, tolerating a 0x prefix and whitespace', () => {
    expect(tryParseAddr('DEADBEEF')).toBe(0xdeadbeef);
    expect(tryParseAddr('0xdeadbeef')).toBe(0xdeadbeef);
    expect(tryParseAddr('  12345678 ')).toBe(0x12345678);
    expect(tryParseAddr('0')).toBe(0);
  });

  it('returns null for empty or non-hex input', () => {
    expect(tryParseAddr('')).toBeNull();
    expect(tryParseAddr('   ')).toBeNull();
    expect(tryParseAddr('0x')).toBeNull();
    expect(tryParseAddr('zzz')).toBeNull();
  });

  it('rejects trailing non-hex garbage rather than truncating it', () => {
    expect(tryParseAddr('abcz')).toBeNull();
    expect(tryParseAddr('12g')).toBeNull();
  });

  it('returns null for addresses beyond the 32-bit range', () => {
    expect(tryParseAddr('100000000')).toBeNull();
    expect(tryParseAddr('FFFFFFFFF')).toBeNull();
  });

  it('accepts the top of the 32-bit range', () => {
    expect(tryParseAddr('FFFFFFFF')).toBe(0xffffffff);
  });
});
