import { describe, it, expect } from 'vitest';
import {
  unpackFramebuffer,
  unpackFramebufferSized,
  FB_WIDTH,
  FB_HEIGHT,
  FB_ROW_BYTES,
  FB_BYTES,
  OLED_WIDTH,
  OLED_HEIGHT,
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

describe('unpackFramebufferSized at OLED geometry (128x64)', () => {
  const OLED_STRIDE = OLED_WIDTH >> 3; // 16 bytes/row, no pad bits
  const OLED_BYTES = OLED_STRIDE * OLED_HEIGHT; // 1024
  const LIT: [number, number, number] = [0xe6, 0xf1, 0xff];
  const OFF: [number, number, number] = [0x06, 0x0a, 0x10];

  it('rejects an undersized OLED payload', () => {
    expect(
      unpackFramebufferSized(b64(new Uint8Array(OLED_BYTES - 1)), OLED_WIDTH, OLED_HEIGHT, LIT, OFF),
    ).toBeNull();
  });

  it('produces an RGBA buffer of the OLED panel size', () => {
    const rgba = unpackFramebufferSized(b64(new Uint8Array(OLED_BYTES)), OLED_WIDTH, OLED_HEIGHT, LIT, OFF);
    expect(rgba).not.toBeNull();
    expect(rgba!.length).toBe(OLED_WIDTH * OLED_HEIGHT * 4);
  });

  it('lights a set bit (foreground) and leaves the rest as the dark substrate', () => {
    const raw = new Uint8Array(OLED_BYTES);
    // row 2, column 0 -> byte at 2*stride, MSB set
    raw[2 * OLED_STRIDE] = 0x80;
    const rgba = unpackFramebufferSized(b64(raw), OLED_WIDTH, OLED_HEIGHT, LIT, OFF)!;
    const lit = (2 * OLED_WIDTH + 0) * 4;
    expect(rgba[lit]).toBe(LIT[0]); // set bit -> lit foreground (bright)
    expect(rgba[lit + 2]).toBe(LIT[2]);
    // top-left pixel was never set: dark substrate
    expect(rgba[0]).toBe(OFF[0]);
    expect(rgba[2]).toBe(OFF[2]);
  });
});
