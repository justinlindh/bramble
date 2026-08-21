import { describe, it, expect } from 'vitest';
import { normalizeAddress } from '../LocationSection';

// normalizeAddress delegates to lib/addr's tryParseAddr, so these cases mirror
// that validator's contract and guard the delegation: the form must accept the
// same addresses tryParseAddr accepts and reject the same malformed input,
// while still producing the canonical padded, uppercase hex string a rule stores.
describe('normalizeAddress', () => {
  it('normalizes valid hex to a padded, uppercase 8-char address', () => {
    expect(normalizeAddress('1a2b3c4d')).toBe('1A2B3C4D');
    expect(normalizeAddress('DEADBEEF')).toBe('DEADBEEF');
    expect(normalizeAddress('1a')).toBe('0000001A');
    expect(normalizeAddress('0x1a')).toBe('0000001A');
    expect(normalizeAddress('  1a2b3c4d ')).toBe('1A2B3C4D');
  });

  it('keeps the legitimate zero address distinct from a rejection', () => {
    expect(normalizeAddress('0')).toBe('00000000');
  });

  it('rejects malformed input instead of truncating it', () => {
    expect(normalizeAddress('')).toBeNull();
    expect(normalizeAddress('   ')).toBeNull();
    expect(normalizeAddress('0x')).toBeNull();
    expect(normalizeAddress('abcz')).toBeNull();
    // Nine hex digits overflow the 32-bit address range; a bare parseInt would
    // have accepted them, tryParseAddr rejects them.
    expect(normalizeAddress('123456789')).toBeNull();
  });
});
