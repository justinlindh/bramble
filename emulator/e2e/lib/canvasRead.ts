// canvasRead.ts
//
// Reads back the ACTUAL rendered pixels of a pager's e-paper <canvas> (what a
// human looking at the browser would see), independent of the React app's
// internal state. Used to compare against the wire-decoded framebuffer
// (fbWire.ts) and to classify full-refresh flash frames (epaperModel.ts's
// black/white inversion flash).

import type { Page } from '@playwright/test';
import { FB_HEIGHT, FB_WIDTH, type BitGrid } from './fbWire';

// Ink/paper midpoint: real content pixels are either near-black ink
// (0x1a,0x1c,0x18) or near-white paper (0xdd,0xdc,0xd2); the inversion-flash
// fills are pure black (#111310) or paper-colored white (#dddcd2). 128 sits
// cleanly between both pairs on every channel, so a single-channel threshold
// classifies all four fill colors correctly.
const INK_THRESHOLD = 128;

export function canvasSelector(nodeId: string): string {
  return `[data-testid="device-card-${nodeId}"] canvas[data-testid="epaper-canvas"]`;
}

// readCanvasRGBA pulls the raw backing-store pixels (FB_WIDTH x FB_HEIGHT,
// RGBA) off a node's epaper canvas via a real 2D context readback (not
// screenshot decoding), so it reflects exactly what putImageData last drew.
export async function readCanvasRGBA(page: Page, nodeId: string): Promise<number[]> {
  return page.$eval(canvasSelector(nodeId), (canvas: HTMLCanvasElement) => {
    const ctx = canvas.getContext('2d');
    if (!ctx) throw new Error('canvasRead: no 2d context');
    const img = ctx.getImageData(0, 0, canvas.width, canvas.height);
    return Array.from(img.data);
  });
}

// toGrid classifies each pixel as ink (true) or paper (false) by the red
// channel against INK_THRESHOLD, and reshapes into a [y][x] BitGrid matching
// fbWire.ts's shape so the two can be compared directly.
export function toGrid(rgba: number[]): BitGrid {
  const grid: BitGrid = [];
  for (let y = 0; y < FB_HEIGHT; y++) {
    const row: boolean[] = new Array(FB_WIDTH);
    for (let x = 0; x < FB_WIDTH; x++) {
      const o = (y * FB_WIDTH + x) * 4;
      row[x] = rgba[o] < INK_THRESHOLD;
    }
    grid.push(row);
  }
  return grid;
}

export async function readCanvasGrid(page: Page, nodeId: string): Promise<BitGrid> {
  return toGrid(await readCanvasRGBA(page, nodeId));
}

export type FillClass = 'black' | 'white' | 'mixed' | 'unpainted';

// A never-painted canvas's backing store is fully-transparent RGBA (0,0,0,0):
// alpha 0. Any putImageData call the app makes (flash fills and real content
// alike) always writes fully-opaque pixels (alpha 255), so a small alpha
// threshold cleanly separates "nothing has been drawn here yet" from a real
// painted frame, including a genuine black flash fill (#111310, alpha 255)
// which otherwise reads identically to blank transparent black on the red
// channel alone.
const ALPHA_THRESHOLD = 8;

// classifyFill reports whether a frame is a uniform inversion-flash fill
// (all-black or all-white), genuine mixed content (ink and paper pixels both
// present, i.e. real rendered glyphs/UI), or 'unpainted' -- the canvas's
// pristine pre-paint state, which must never be mistaken for a real black
// flash (see canvasRead.ts module comment / display-correctness.spec.ts).
export function classifyFill(rgba: number[]): FillClass {
  let sawInk = false;
  let sawPaper = false;
  let sawPainted = false;
  for (let i = 0; i < rgba.length; i += 4) {
    if (rgba[i + 3] < ALPHA_THRESHOLD) continue; // transparent: not yet painted
    sawPainted = true;
    if (rgba[i] < INK_THRESHOLD) sawInk = true;
    else sawPaper = true;
    if (sawInk && sawPaper) return 'mixed';
  }
  if (!sawPainted) return 'unpainted';
  return sawInk ? 'black' : 'white';
}
