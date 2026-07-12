import { describe, it, expect } from 'vitest';
import {
  unpackFramebuffer,
  FB_WIDTH,
  FB_HEIGHT,
  FB_ROW_BYTES,
  FB_BYTES,
} from './framebuffer';

function b64(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}

describe('unpackFramebuffer', () => {
  it('rejects an undersized payload', () => {
    expect(unpackFramebuffer(b64(new Uint8Array(FB_BYTES - 1)))).toBeNull();
  });

  it('produces an RGBA buffer of the panel size', () => {
    const rgba = unpackFramebuffer(b64(new Uint8Array(FB_BYTES)));
    expect(rgba).not.toBeNull();
    expect(rgba!.length).toBe(FB_WIDTH * FB_HEIGHT * 4);
  });

  it('maps bit 7 of byte 0 to the top-left pixel (ink)', () => {
    const raw = new Uint8Array(FB_BYTES);
    raw[0] = 0x80; // MSB set -> top-left inked
    const rgba = unpackFramebuffer(b64(raw))!;
    // top-left pixel is dark (ink), pixel to its right is paper (light)
    expect(rgba[0]).toBeLessThan(0x40);
    expect(rgba[4]).toBeGreaterThan(0xc0);
  });

  it('places a set bit at the correct row/column (row-major, 32 bytes/row)', () => {
    const raw = new Uint8Array(FB_BYTES);
    // row 1, column 0 -> byte at FB_ROW_BYTES, bit 7
    raw[FB_ROW_BYTES] = 0x80;
    const rgba = unpackFramebuffer(b64(raw))!;
    const o = (1 * FB_WIDTH + 0) * 4;
    expect(rgba[o]).toBeLessThan(0x40); // inked
    expect(rgba[0]).toBeGreaterThan(0xc0); // (0,0) still paper
  });
});
