import { describe, it, expect } from 'vitest';
import { formatAddrHex, formatAddr0x, formatAddrShort } from '../address';

describe('address formatting', () => {
  it('pads to 8 uppercase hex chars', () => {
    expect(formatAddrHex(0x1234)).toBe('00001234');
    expect(formatAddr0x(0x1234)).toBe('0x00001234');
    expect(formatAddrShort(0x1234)).toBe('0x1234');
  });

  it('normalizes high-bit addresses to uint32 instead of stringifying as negative', () => {
    // 0xDEADBEEF exceeds 2^31, so a signed interpretation would render as
    // a negative number without the `>>> 0` guard.
    expect(formatAddrHex(0xdeadbeef)).toBe('DEADBEEF');
    expect(formatAddr0x(0xdeadbeef)).toBe('0xDEADBEEF');
    expect(formatAddrShort(0xdeadbeef)).toBe('0xBEEF');
  });
});
