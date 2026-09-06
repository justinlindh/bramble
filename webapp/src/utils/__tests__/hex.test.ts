import { describe, it, expect } from 'vitest';
import { HEX64, normalizeHex64 } from '../hex';

const KEY_UPPER = 'AB'.repeat(32); // 64 hex characters
const KEY_LOWER = 'ab'.repeat(32);

describe('HEX64', () => {
  it('matches exactly 64 hex characters', () => {
    expect(HEX64.test(KEY_LOWER)).toBe(true);
    expect(HEX64.test(KEY_UPPER)).toBe(true);
  });

  it('rejects the wrong length or non-hex characters', () => {
    expect(HEX64.test('ab'.repeat(31) + 'a')).toBe(false); // 63 chars
    expect(HEX64.test(KEY_LOWER + 'a')).toBe(false); // 65 chars
    expect(HEX64.test('g'.repeat(64))).toBe(false); // non-hex
  });
});

describe('normalizeHex64', () => {
  it('lowercases a valid 64-hex key', () => {
    expect(normalizeHex64(KEY_UPPER)).toBe(KEY_LOWER);
  });

  it('trims surrounding whitespace before validating', () => {
    expect(normalizeHex64(`  ${KEY_UPPER}\n`)).toBe(KEY_LOWER);
  });

  it('returns null for anything that is not 64 hex characters', () => {
    expect(normalizeHex64('')).toBeNull();
    expect(normalizeHex64(KEY_LOWER.slice(0, 63))).toBeNull();
    expect(normalizeHex64(`${KEY_LOWER}0`)).toBeNull();
    expect(normalizeHex64('z'.repeat(64))).toBeNull();
  });
});
