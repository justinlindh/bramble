// glyphMatch.ts
//
// Given an ASCII string, rasterize it with the exact blit rule
// display_draw_text/display_draw_text_large use (font6x8.ts) and slide the
// result over a decoded bit grid (fbWire.ts's BitGrid, or a canvas readback
// turned into the same shape) to find a pixel-exact occurrence. This is an
// independent TypeScript reimplementation of the same technique
// simulator/gosim/screen_assert.go uses to gate the headless scenario suite
// (Task 10); the emulator/e2e suite asserts the same thing at the browser
// layer, so it is written fresh rather than shared, on purpose.

import { FONT_FIRST_CODE, FONT_LAST_CODE, font6x8 } from './font6x8';
import { FB_HEIGHT, FB_WIDTH, type BitGrid } from './fbWire';

export type GlyphPattern = boolean[][]; // pattern[col][row], row 0 = top

// renderGlyphs rasterizes `text` into a width x 8*scale boolean pattern.
// scale=1 mirrors display_draw_text (6px advance); scale=2 mirrors
// display_draw_text_large (12px advance, each source pixel drawn as a 2x2
// block, per display_virt.c's display_draw_text_large). Non-printable
// characters render as a blank (unlit) cell but still advance, matching the
// firmware. Returns ok=false if the string can never fit on the panel or
// contains no ink (never treat "found nothing" as a match).
export function renderGlyphs(text: string, scale: 1 | 2 = 1): { pattern: GlyphPattern; ok: boolean } {
  const cellW = 6 * scale;
  const cellH = 8 * scale;
  const w = cellW * text.length;
  if (text.length === 0 || w > FB_WIDTH) {
    return { pattern: [], ok: false };
  }
  const pattern: GlyphPattern = Array.from({ length: w }, () => new Array(cellH).fill(false));
  let anyInk = false;
  for (let i = 0; i < text.length; i++) {
    const code = text.charCodeAt(i);
    if (code < FONT_FIRST_CODE || code > FONT_LAST_CODE) continue; // blank cell, still advances
    const glyph = font6x8[code - FONT_FIRST_CODE];
    for (let col = 0; col < 6; col++) {
      const bits = glyph[col];
      for (let row = 0; row < 8; row++) {
        if ((bits & (1 << row)) === 0) continue;
        anyInk = true;
        if (scale === 1) {
          pattern[i * cellW + col][row] = true;
        } else {
          const px = i * cellW + col * 2;
          const py = row * 2;
          pattern[px][py] = true;
          pattern[px + 1][py] = true;
          pattern[px][py + 1] = true;
          pattern[px + 1][py + 1] = true;
        }
      }
    }
  }
  return { pattern, ok: anyInk };
}

function pixelAt(grid: BitGrid, x: number, y: number): boolean {
  if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return false;
  return grid[y]?.[x] ?? false;
}

function boxMatches(grid: BitGrid, pattern: GlyphPattern, ox: number, oy: number, invert: boolean): boolean {
  const w = pattern.length;
  const h = pattern[0]?.length ?? 0;
  for (let px = 0; px < w; px++) {
    for (let py = 0; py < h; py++) {
      let ink = pixelAt(grid, ox + px, oy + py);
      if (invert) ink = !ink;
      if (ink !== pattern[px][py]) return false;
    }
  }
  return true;
}

export interface MatchResult {
  found: boolean;
  x?: number;
  y?: number;
  inverted?: boolean;
}

// slideMatch searches every offset in `grid` for a pixel-exact occurrence of
// `pattern`, trying both ink polarities (dark-on-light and light-on-dark), so
// the assertion doesn't depend on which screen background the text landed on.
export function slideMatch(grid: BitGrid, pattern: GlyphPattern): MatchResult {
  const w = pattern.length;
  const h = pattern[0]?.length ?? 0;
  if (w === 0 || h === 0 || w > FB_WIDTH || h > FB_HEIGHT) return { found: false };
  for (const invert of [false, true]) {
    for (let oy = 0; oy <= FB_HEIGHT - h; oy++) {
      for (let ox = 0; ox <= FB_WIDTH - w; ox++) {
        if (boxMatches(grid, pattern, ox, oy, invert)) {
          return { found: true, x: ox, y: oy, inverted: invert };
        }
      }
    }
  }
  return { found: false };
}

// findText is the one-call convenience form: render `text` at `scale` and
// search for it in `grid`.
export function findText(grid: BitGrid, text: string, scale: 1 | 2 = 1): MatchResult {
  const { pattern, ok } = renderGlyphs(text, scale);
  if (!ok) return { found: false };
  return slideMatch(grid, pattern);
}
