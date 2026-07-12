import { describe, it, expect } from 'vitest';
import {
  EPD_MODEL,
  initialEpaperState,
  applyFrame,
} from './epaperModel';

describe('epaperModel EPD_MODEL table', () => {
  it('exposes the datasheet-seeded tunables', () => {
    expect(EPD_MODEL.ghostPerFrame).toBeCloseTo(0.06);
    expect(EPD_MODEL.ghostMax).toBeCloseTo(0.3);
    expect(EPD_MODEL.flashColors).toEqual(['black', 'white', 'black', 'white']);
  });
});

describe('epaperModel partial refresh', () => {
  it('emits a single content frame after busy_ms', () => {
    const { frames, ghost } = applyFrame(initialEpaperState(), 'AAAA', 'partial', 300);
    expect(frames).toHaveLength(1);
    expect(frames[0].at).toBe(300);
    expect(frames[0].paint).toBe('content');
    expect(frames[0].ghost).toBeCloseTo(0.06);
    expect(ghost).toBeCloseTo(0.06);
  });

  it('accumulates ghost +0.06 per partial frame', () => {
    let state = initialEpaperState();
    const seen: number[] = [];
    for (let i = 0; i < 4; i++) {
      const r = applyFrame(state, 'x', 'partial', 120);
      state = { ghost: r.ghost };
      seen.push(r.ghost);
    }
    expect(seen[0]).toBeCloseTo(0.06);
    expect(seen[1]).toBeCloseTo(0.12);
    expect(seen[2]).toBeCloseTo(0.18);
    expect(seen[3]).toBeCloseTo(0.24);
  });

  it('caps ghost accumulation at ghostMax (0.3)', () => {
    let state = initialEpaperState();
    for (let i = 0; i < 20; i++) {
      state = { ghost: applyFrame(state, 'x', 'partial', 100).ghost };
    }
    expect(state.ghost).toBeCloseTo(0.3);
    // one more must not exceed the cap
    const r = applyFrame(state, 'x', 'partial', 100);
    expect(r.ghost).toBeCloseTo(0.3);
    expect(r.frames[0].ghost).toBeCloseTo(0.3);
  });
});

describe('epaperModel full refresh', () => {
  it('plays a rapid black/white inversion flicker then content', () => {
    const { frames, ghost } = applyFrame(initialEpaperState(), 'x', 'full', 2600);
    // Four inversions at the fixed flash step (130ms), content latched right
    // after -- NOT spread across the 2600ms busy window.
    expect(frames).toHaveLength(5);
    expect(frames.map((f) => f.paint)).toEqual(['black', 'white', 'black', 'white', 'content']);
    expect(frames.map((f) => f.at)).toEqual([0, 130, 260, 390, 520]);
    expect(ghost).toBe(0);
  });

  it('resets accumulated ghost to zero', () => {
    let state = initialEpaperState();
    for (let i = 0; i < 6; i++) {
      state = { ghost: applyFrame(state, 'x', 'partial', 100).ghost };
    }
    expect(state.ghost).toBeGreaterThan(0);
    const r = applyFrame(state, 'x', 'full', 1800);
    expect(r.ghost).toBe(0);
    // the content frame that lands at the end carries no residual ghost
    const content = r.frames[r.frames.length - 1];
    expect(content.paint).toBe('content');
    expect(content.ghost).toBe(0);
  });

  it('plays the flash at a fixed step independent of busy_ms', () => {
    // The visible flicker must not stretch with the busy window: a long busy_ms
    // was the cause of the screen holding solid black for over a second.
    const short = applyFrame(initialEpaperState(), 'x', 'full', 500);
    const long = applyFrame(initialEpaperState(), 'x', 'full', 4000);
    const expected = [0, 130, 260, 390, 520];
    expect(short.frames.map((f) => f.at)).toEqual(expected);
    expect(long.frames.map((f) => f.at)).toEqual(expected);
  });
});
