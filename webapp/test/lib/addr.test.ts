import { describe, expect, it } from 'vitest';
import { parseAddr } from '../../src/lib/addr';

describe('parseAddr', () => {
  it('parses an uppercase hex string', () => {
    expect(parseAddr('F2BE6EEE')).toBe(0xF2BE6EEE);
  });

  it('parses a lowercase hex string', () => {
    expect(parseAddr('a1b2c3d4')).toBe(0xA1B2C3D4);
  });

  it('parses a hex string without leading zeros', () => {
    expect(parseAddr('ff')).toBe(0xFF);
  });

  it('passes a number through unchanged', () => {
    expect(parseAddr(0x1A3F2B4C)).toBe(0x1A3F2B4C);
  });

  it('passes zero through unchanged', () => {
    expect(parseAddr(0)).toBe(0);
  });

  it('defaults undefined to 0', () => {
    expect(parseAddr(undefined)).toBe(0);
  });
});
